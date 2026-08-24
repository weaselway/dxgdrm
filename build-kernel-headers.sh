#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# WSL ships no /lib/modules/$(uname -r)/build, and the running kernel is
# Microsoft's, not the distro's -- there is no linux-headers package to install.
# Building anything out-of-tree (kernel/dxgdrm) means fetching the matching
# source and preparing it ourselves.
#
# Override to reuse a tree you already have:
#   KERNEL_SRC=~/dev/WSL2-Linux-Kernel ./build-kernel-headers.sh
KERNEL_SRC="${KERNEL_SRC:-${SCRIPT_DIR}/build/wsl-kernel}"

KERNEL_REPO=https://github.com/microsoft/WSL2-Linux-Kernel.git
KERNEL_RELEASE="$(uname -r)"          # 6.18.33.2-microsoft-standard-WSL2
KERNEL_VERSION="${KERNEL_RELEASE%%-*}"  # 6.18.33.2
KERNEL_TAG="linux-msft-wsl-${KERNEL_VERSION}"

# The prepared tree is only good for the kernel it was configured against, so
# the stamp records which one. A WSL update moves uname -r and invalidates it.
STAMP="${KERNEL_SRC}/.headers-ready"

if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${KERNEL_RELEASE}" ]; then
  echo "build-kernel-headers.sh: ${KERNEL_SRC} already prepared for ${KERNEL_RELEASE}"
  exit 0
fi

missing=()
for tool in git gcc make flex bison bc pahole rsync openssl; do
  command -v "${tool}" >/dev/null || missing+=("${tool}")
done
# elfutils' libelf headers have no binary to probe for.
[ -e /usr/include/libelf.h ] || missing+=("libelf.h")
if [ ${#missing[@]} -ne 0 ]; then
  echo "build-kernel-headers.sh: missing build dependencies: ${missing[*]}" >&2
  echo "  sudo dnf install -y bc openssl-devel elfutils-libelf-devel dwarves rsync flex bison" >&2
  exit 1
fi

if [ ! -d "${KERNEL_SRC}/.git" ]; then
  # Confirm the tag exists before a multi-hundred-megabyte clone fails on it.
  # Microsoft tags every servicing release, so an exact match is the norm; when
  # it is missing the kernel is newer than the published tags.
  if ! git ls-remote --tags --exit-code "${KERNEL_REPO}" "refs/tags/${KERNEL_TAG}" >/dev/null 2>&1; then
    echo "build-kernel-headers.sh: no tag ${KERNEL_TAG} in ${KERNEL_REPO}" >&2
    echo "  running kernel is ${KERNEL_RELEASE}; published tags near it:" >&2
    git ls-remote --tags "${KERNEL_REPO}" 2>/dev/null \
      | grep -oE 'linux-msft-wsl-[0-9.]+' | sort -uV | tail -5 | sed 's/^/    /' >&2
    exit 1
  fi

  echo "build-kernel-headers.sh: cloning ${KERNEL_TAG}"
  mkdir -p "$(dirname "${KERNEL_SRC}")"
  git clone --depth 1 --branch "${KERNEL_TAG}" --single-branch \
    "${KERNEL_REPO}" "${KERNEL_SRC}"
fi

# LOCALVERSION must be set, to empty, on every invocation. Left unset,
# scripts/setlocalversion appends "+" for a tree that is not sitting on an
# annotated tag -- which a shallow clone is not -- and that "+" lands in
# vermagic. insmod then rejects the module against a kernel built without it,
# with no hint as to why.
KMAKE=(make -C "${KERNEL_SRC}" LOCALVERSION= -j"$(nproc)")

# Use the config Microsoft ships in the tree, as the kernel README does. It is
# the config that built this tag, complete down to the CC_HAS_* autodetects, so
# there is nothing to fill in and no explicit olddefconfig step: kbuild runs
# syncconfig itself on the way to any target.
#
# CONFIG_WERROR is already unset in it, so nothing needs disabling to survive a
# newer distro compiler than the gcc 13.2 that built the release.
#
# Note what is deliberately NOT trimmed. Disabling CONFIG_DEBUG_INFO_BTF and
# CONFIG_DEBUG_INFO_BTF_MODULES roughly halves this build, and it is the wrong
# trade: those add four fields to struct module, so the kernel rejects the
# resulting module outright --
#   .gnu.linkonce.this_module section size must match the kernel's built
#   struct module size at run time
KCONFIG_SRC="${KERNEL_SRC}/Microsoft/config-wsl"
if [ ! -r "${KCONFIG_SRC}" ]; then
  echo "build-kernel-headers.sh: ${KCONFIG_SRC} is missing" >&2
  exit 1
fi

echo "build-kernel-headers.sh: configuring from Microsoft/config-wsl"
cp "${KCONFIG_SRC}" "${KERNEL_SRC}/.config"

# The in-tree config is not regenerated for every servicing release -- at
# 6.18.33.2 it still carries a "6.18.20.1" header -- so it tracks the tag, not
# necessarily the kernel actually running. Identical in practice, but worth
# checking rather than assuming, because the failure it would cause (a module
# built against the wrong struct layouts) is opaque. Only advisory: booting a
# custom kernel is a legitimate reason to differ.
if [ -r /proc/config.gz ]; then
  if ! diff -q <(zcat /proc/config.gz | grep -E '^(CONFIG_|# CONFIG_)' | sort) \
                <(grep -E '^(CONFIG_|# CONFIG_)' "${KCONFIG_SRC}" | sort) >/dev/null; then
    echo "build-kernel-headers.sh: WARNING: the running kernel's config differs from" >&2
    echo "  ${KCONFIG_SRC}. Building against the in-tree config anyway; if the module" >&2
    echo "  is rejected, use the running config instead:" >&2
    echo "    zcat /proc/config.gz > ${KERNEL_SRC}/.config" >&2
  fi
fi

echo "build-kernel-headers.sh: building vmlinux (slow -- this is where the CRCs come from)"
"${KMAKE[@]}" vmlinux

cp "${KERNEL_SRC}/vmlinux.symvers" "${KERNEL_SRC}/Module.symvers"

echo "build-kernel-headers.sh: preparing module build support"
"${KMAKE[@]}" modules_prepare

# Sanity-check the thing that silently breaks everything downstream.
built_release="$(cat "${KERNEL_SRC}/include/config/kernel.release")"
if [ "${built_release}" != "${KERNEL_RELEASE}" ]; then
  echo "build-kernel-headers.sh: prepared tree reports '${built_release}'," >&2
  echo "  but the running kernel is '${KERNEL_RELEASE}' -- vermagic will not match." >&2
  exit 1
fi

echo "${KERNEL_RELEASE}" > "${STAMP}"

cat <<EOF

build-kernel-headers.sh: ${KERNEL_SRC} ready for ${KERNEL_RELEASE}

Build and load the module with:
  make -C kernel/dxgdrm load

One caveat that this script cannot fix. MODVERSIONS CRCs depend on the
compiler, and Fedora ships no gcc 13.2 to match Microsoft's build, so the
module needs 'modprobe --force-modversion' and taints the kernel. The struct
layouts do match -- CONFIG_RANDSTRUCT_NONE, and the config differs only in
compiler-capability autodetects -- so this is safe, but it is not a deployment
story. The clean fix is to boot the vmlinux just built, via kernel= in
.wslconfig.
EOF
