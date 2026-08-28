#!/usr/bin/env bash
set -euo pipefail

# Builds the WSL kernel headers and dxgdrm.ko inside an Ubuntu container. It
# stops there: loading the module is a separate, explicit step, and one that
# has to happen on the host -- it loads into the kernel the container shares.
#
# The kernel source/build tree (build-kernel-headers.sh's KERNEL_SRC) lands in
# ./build, which is bind-mounted so it survives the container and is reused on
# the next run. Nothing else is mounted: the build needs no root and writes
# nothing outside this directory. That the container shares the host's kernel
# still matters, though -- it is what makes uname -r, and therefore
# KERNEL_RELEASE in build-kernel-headers.sh, the release we are building for.
#
# Usage:
#   ./docker-env.sh                 # build headers + dxgdrm.ko
#   ./docker-env.sh make clean      # or any other make target
#   ./docker-env.sh bash            # interactive shell in the build environment

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONTAINER_WORKDIR=/work

# The container runs as the invoking user so the kernel tree and dxgdrm.ko come
# back owned by them rather than by root. The image has to be built for those
# ids too (the Dockerfile bakes in a matching /etc/passwd entry), so the tag
# carries the uid -- otherwise a second user on the same docker daemon would
# silently reuse an image whose account does not match.
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"
IMAGE="wsl/dxgdrm-build:uid${HOST_UID}"

mkdir -p "${SCRIPT_DIR}/build"

docker build \
  --build-arg UID="${HOST_UID}" \
  --build-arg GID="${HOST_GID}" \
  -t "${IMAGE}" "${SCRIPT_DIR}"

DOCKER_RUN=(docker run --rm -it
  --user "${HOST_UID}:${HOST_GID}"
  -v "${SCRIPT_DIR}:${CONTAINER_WORKDIR}"
  -w "${CONTAINER_WORKDIR}"
  "${IMAGE}")

if [ $# -gt 0 ]; then
  exec "${DOCKER_RUN[@]}" "$@"
fi

"${DOCKER_RUN[@]}" bash -c './build-kernel-headers.sh && make all'

cat <<'EOF'

docker-env.sh: dxgdrm.ko built. Load it with:

  sudo modprobe ./dxgdrm.ko
  sudo udevadm trigger --subsystem-match=drm
  sudo udevadm settle

The module is loaded out of this directory rather than installed first: on WSL
/lib/modules is an overlay that WSL itself mounts, and anything written there
is gone at the next `wsl --shutdown`. See the comment in the Makefile.

EOF
