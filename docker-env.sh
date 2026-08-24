#!/usr/bin/env bash
set -euo pipefail

# Builds the WSL kernel headers and dxgdrm.ko inside a Fedora container, then
# installs the module the same way `make install` does on the host.
#
# The kernel source/build tree (build-kernel-headers.sh's KERNEL_SRC) lands in
# ./build, which is bind-mounted so it survives the container and is reused
# on the next run. /lib/modules is bind-mounted too, because `make install`
# writes into /lib/modules/$(uname -r)/extra and runs depmod -- and since
# containers share the host's kernel, uname -r inside the container matches
# the host, so that write lands in the right place.
#
# Usage:
#   ./docker-env.sh              # build headers + module, then `make install`
#   ./docker-env.sh bash         # interactive shell in the build environment
#   ./docker-env.sh make load    # run an arbitrary command instead

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE=wsl/dxgdrm-build
CONTAINER_WORKDIR=/work

mkdir -p "${SCRIPT_DIR}/build"

docker build -t "${IMAGE}" "${SCRIPT_DIR}"

if [ $# -eq 0 ]; then
  set -- bash -c './build-kernel-headers.sh && make install'
fi

exec docker run --rm -it \
  -v "${SCRIPT_DIR}:${CONTAINER_WORKDIR}" \
  -v /lib/modules:/lib/modules \
  -w "${CONTAINER_WORKDIR}" \
  "${IMAGE}" \
  "$@"
