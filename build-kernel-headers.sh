#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# WSL ships no /lib/modules/$(uname -r)/build, and the running kernel is
# Microsoft's, not the distro's -- there is no linux-headers package to install.
# Building dxgdrm out-of-tree means fetching the matching source and preparing
# it ourselves.
#
# Override to reuse a tree you already have:
#   KERNEL_SRC=~/dev/WSL2-Linux-Kernel ./build-kernel-headers.sh
KERNEL_SRC="${KERNEL_SRC:-${SCRIPT_DIR}/build/wsl-kernel}"

# Nothing here needs root: the clone, the kernel build and the stamp all land
# in KERNEL_SRC, which belongs to whoever runs this -- docker-env.sh runs the
# container as the host user for exactly that reason. Only `make load` needs
# root, to talk to the kernel, and that sudo lives in the Makefile.
KERNEL_REPO=https://github.com/microsoft/WSL2-Linux-Kernel.git

# Which kernel to build against. On WSL that is the running one, and its config
# comes from the source tree. Off WSL -- CI, or any machine not running the
# target kernel -- uname -r says nothing useful, so point KERNEL_CONFIG at one
# of the captured configs in ./conf instead:
#
#   KERNEL_CONFIG=conf/kernel-6.18.33.2-microsoft-standard-WSL2.conf \
#     ./build-kernel-headers.sh
#
# Those are named for the release they were captured from, which is where the
# release then comes from -- the config file itself does not carry it (the
# version is the tree's, the "-microsoft-standard-WSL2" suffix is
# CONFIG_LOCALVERSION, and nothing states the pair). Deriving it from the name
# is not taken on trust: the built tree's kernel.release is checked against it
# at the end, which is the same check the WSL path relies on.
KERNEL_CONFIG="${KERNEL_CONFIG:-}"

if [ -n "${KERNEL_CONFIG}" ]; then
  config_base="$(basename "${KERNEL_CONFIG}")"
  case "${config_base}" in
    kernel-*.conf) ;;
    *)
      echo "build-kernel-headers.sh: KERNEL_CONFIG must be named" >&2
      echo "  kernel-<release>.conf, not '${config_base}'" >&2
      exit 1
      ;;
  esac
  config_release="${config_base#kernel-}"
  config_release="${config_release%.conf}"
  KERNEL_RELEASE="${KERNEL_RELEASE:-${config_release}}"
else
  KERNEL_RELEASE="${KERNEL_RELEASE:-$(uname -r)}"  # 6.18.33.2-microsoft-standard-WSL2
fi

KERNEL_VERSION="${KERNEL_RELEASE%%-*}"  # 6.18.33.2
KERNEL_TAG="linux-msft-wsl-${KERNEL_VERSION}"

# The prepared tree is only good for the kernel it was configured against, so
# the stamp records which one. A WSL update moves uname -r and invalidates it.
STAMP="${KERNEL_SRC}/.headers-ready"

if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${KERNEL_RELEASE}" ]; then
  echo "build-kernel-headers.sh: ${KERNEL_SRC} already prepared for ${KERNEL_RELEASE}"
  exit 0
fi

if [ ! -d "${KERNEL_SRC}/.git" ]; then
  # Confirm the tag exists before a multi-hundred-megabyte clone fails on it.
  # Microsoft tags every servicing release, so an exact match is the norm; when
  # it is missing the kernel is newer than the published tags.
  if ! git ls-remote --tags --exit-code "${KERNEL_REPO}" "refs/tags/${KERNEL_TAG}" >/dev/null 2>&1; then
    echo "build-kernel-headers.sh: no tag ${KERNEL_TAG} in ${KERNEL_REPO}" >&2
    echo "  target kernel is ${KERNEL_RELEASE}; published tags near it:" >&2
    git ls-remote --tags "${KERNEL_REPO}" 2>/dev/null \
      | grep -oE 'linux-msft-wsl-[0-9.]+' | sort -uV | tail -5 | sed 's/^/    /' >&2
    exit 1
  fi

  echo "build-kernel-headers.sh: cloning ${KERNEL_TAG}"
  mkdir -p "$(dirname "${KERNEL_SRC}")"
  git clone --depth 1 --branch "${KERNEL_TAG}" --single-branch \
    "${KERNEL_REPO}" "${KERNEL_SRC}"
fi

# MODVERSIONS CRCs depend on the compiler, so a module built with a different
# one is rejected with "disagrees about version of symbol module_layout" and
# needs modprobe --force-modversion, which taints the kernel.
#
# /proc/version and CONFIG_CC_VERSION_TEXT both report the running kernel was
# built with "gcc (GCC) 13.2.0" -- bare "(GCC)", i.e. a vanilla upstream build
# with --with-pkgversion left at its default. No distro compiler looks like
# that: Ubuntu stamps "(Ubuntu 13.2.0-23ubuntu4)", Fedora 39 shipped 13.2.1
# stamped "(Red Hat 13.2.1-6)". The kernel.org crosstool builds do, which is
# why the toolchain comes from there.
#
# It lands in ./build next to the kernel tree -- both are build products, both
# are gitignored, and both survive the container because that directory is
# bind-mounted. Setting KGCC in the environment overrides all of this and skips
# the download, for a toolchain that is already on the machine.
KGCC_VERSION="${KGCC_VERSION:-13.2.0}"
# Recorded when the version was pinned; overriding KGCC_VERSION means passing
# its KGCC_SHA256 as well.
if [ "${KGCC_VERSION}" = 13.2.0 ]; then
    KGCC_SHA256="${KGCC_SHA256:-2f1975496a15200f2aaef4e0998c530bbcba6ca237285be76f030087da563fb6}"
fi
KGCC_DIR="${SCRIPT_DIR}/build/kgcc"
KGCC_STAMP="${KGCC_DIR}/.version"

KGCC="${KGCC_DIR}/bin/x86_64-linux-gcc"

# Same shape as the kernel stamp above: the directory alone does not say
# which version is in it, so a KGCC_VERSION bump has to invalidate it.
if [ ! -x "${KGCC}" ] || [ "$(cat "${KGCC_STAMP}" 2>/dev/null)" != "${KGCC_VERSION}" ]; then
    KGCC_URL="https://mirrors.edge.kernel.org/pub/tools/crosstool/files/bin/x86_64/${KGCC_VERSION}/x86_64-gcc-${KGCC_VERSION}-nolibc-x86_64-linux.tar.xz"

    echo "build-kernel-headers.sh: fetching gcc ${KGCC_VERSION} from kernel.org"
    rm -rf "${KGCC_DIR}"
    mkdir -p "${KGCC_DIR}"

    # --strip-components=2 drops the archive's gcc-13.2.0-nolibc/x86_64-linux/
    # prefix; gcc locates its own libexec relative to the binary, so relocating
    # the tree is fine. Running --version is the check that the layout was as
    # expected, so a surprise fails here rather than deep in the kernel build.
    if [ -z "${KGCC_SHA256:-}" ]; then
        echo "build-kernel-headers.sh: set KGCC_SHA256 for gcc ${KGCC_VERSION}" >&2
        exit 1
    fi
    KGCC_TARBALL="${KGCC_DIR}.tar.xz"
    curl -fsSL --retry 3 -o "${KGCC_TARBALL}" "${KGCC_URL}"
    if ! echo "${KGCC_SHA256}  ${KGCC_TARBALL}" | sha256sum -c --quiet -; then
        echo "build-kernel-headers.sh: checksum mismatch for $(basename "${KGCC_URL}")" >&2
        rm -f "${KGCC_TARBALL}"
        exit 1
    fi
    tar -xJf "${KGCC_TARBALL}" -C "${KGCC_DIR}" --strip-components=2
    rm -f "${KGCC_TARBALL}"
    "${KGCC}" --version >/dev/null

    echo "${KGCC_VERSION}" > "${KGCC_STAMP}"
fi

# LOCALVERSION must be set, to empty, on every invocation. Left unset,
# scripts/setlocalversion appends "+" for a tree that is not sitting on an
# annotated tag -- which a shallow clone is not -- and that "+" lands in
# vermagic. insmod then rejects the module against a kernel built without it,
# with no hint as to why.
#
# Only CC comes from the fetched toolchain. Host programs (genksyms and the
# rest of scripts/) keep using the distro gcc: this is a nolibc toolchain and
# cannot build them anyway, and they do not feed the CRCs, which come from the
# target compiler's preprocessor output.
#
# The module build has to agree with this, so the Makefile defaults KGCC to the
# same path.
KMAKE=(make -C "${KERNEL_SRC}" LOCALVERSION= -j"$(nproc)" CC="${KGCC}")

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
#
# KERNEL_CONFIG overrides the source of the config, not any of the above: a
# captured config is a copy of what the target kernel was actually configured
# with, so the same reasoning applies to it unchanged.
KCONFIG_SRC="${KERNEL_CONFIG:-${KERNEL_SRC}/Microsoft/config-wsl}"
if [ ! -r "${KCONFIG_SRC}" ]; then
  echo "build-kernel-headers.sh: ${KCONFIG_SRC} is missing" >&2
  exit 1
fi

echo "build-kernel-headers.sh: configuring from ${KCONFIG_SRC}"
cp "${KCONFIG_SRC}" "${KERNEL_SRC}/.config"

# The in-tree config is not regenerated for every servicing release -- at
# 6.18.33.2 it still carries a "6.18.20.1" header -- so it tracks the tag, not
# necessarily the kernel actually running. Identical in practice, but worth
# checking rather than assuming, because the failure it would cause (a module
# built against the wrong struct layouts) is opaque. Only advisory: booting a
# custom kernel is a legitimate reason to differ.
#
# Only when the target is the kernel we are running on. Building for another
# release -- the KERNEL_CONFIG case -- makes the two configs unrelated, and the
# difference expected rather than a warning.
if [ "${KERNEL_RELEASE}" = "$(uname -r)" ] && [ -r /proc/config.gz ]; then
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
  echo "  but the target kernel is '${KERNEL_RELEASE}' -- vermagic will not match." >&2
  exit 1
fi

echo "${KERNEL_RELEASE}" > "${STAMP}"
