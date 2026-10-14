#!/usr/bin/env bash
set -euo pipefail

# Adds a WSL kernel release to the flake: fetches its in-tree config into
# conf/ and its source hash into `kernels` in flake.nix.
#
#   ./add-kernel.sh                  # the newest linux-msft-wsl-* tag
#   ./add-kernel.sh 6.18.54.1
#   ./add-kernel.sh "$(uname -r)"    # the kernel this WSL runs
#
# The in-tree config is not olddefconfig'd for every release (stale header,
# different order), but its values match /proc/config.gz; the flake's config
# check compares values only.

cd "$(dirname "$0")"
repo=microsoft/WSL2-Linux-Kernel

version="${1:-}"
version="${version%%-*}"
if [ -z "${version}" ]; then
  version="$(git ls-remote --tags --refs "https://github.com/${repo}" 'linux-msft-wsl-*' \
    | sed 's|.*/linux-msft-wsl-||' | sort -V | tail -n1)"
fi
tag="linux-msft-wsl-${version}"

config="$(mktemp)"
trap 'rm -f "${config}"' EXIT
curl -fsSL -o "${config}" "https://raw.githubusercontent.com/${repo}/${tag}/arch/x86/configs/config-wsl"

localversion="$(sed -n 's/^CONFIG_LOCALVERSION="\(.*\)"$/\1/p' "${config}")"
release="${version}${localversion}"
conf="conf/kernel-${release}.conf"

if grep -qF "\"${release}\" = " flake.nix; then
  echo "add-kernel.sh: ${release} is already in flake.nix" >&2
  exit 1
fi

echo "add-kernel.sh: prefetching ${tag} (a few hundred MB)"
hash="$(nix flake prefetch --json "github:${repo}/${tag}" | jq -r .hash)"

cp "${config}" "${conf}"
# The entries are sorted by version; a new release goes last.
sed -i "/^      kernels = {/,/^      };/ s|^      };|        \"${release}\" = \"${hash}\";\n      };|" flake.nix

echo "add-kernel.sh: added ${release} (${conf}, ${hash})"
echo "build it with: nix build '.#\"dxgdrm-${release}\"'"
