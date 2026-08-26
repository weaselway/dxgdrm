#!/usr/bin/env bash
set -euo pipefail

# Builds the WSL kernel headers and dxgdrm.ko inside an Ubuntu container. It
# stops there: installing the module is a separate, explicit step.
#
# The kernel source/build tree (build-kernel-headers.sh's KERNEL_SRC) lands in
# ./build, which is bind-mounted so it survives the container and is reused
# on the next run. /lib/modules is bind-mounted too, for `make install`: it
# writes into /lib/modules/$(uname -r)/extra and runs depmod -- and since
# containers share the host's kernel, uname -r inside the container matches
# the host, so that write lands in the right place.
#
# Running `make install` through here is the reliable path. It depends on `all`,
# so it can rebuild the module, and rebuilding has to use the gcc 13.2.0 that
# produced the CRCs. The Makefile does default KGCC to the copy
# build-kernel-headers.sh installed under ./build, so a host `make install`
# picks up the same compiler -- but only if that toolchain runs on the host,
# which is one more thing to be right about for no gain.
#
# Usage:
#   ./docker-env.sh                 # build headers + dxgdrm.ko
#   ./docker-env.sh make install    # then install it into /lib/modules
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
  -v /lib/modules:/lib/modules
  -w "${CONTAINER_WORKDIR}"
  "${IMAGE}")

if [ $# -gt 0 ]; then
  exec "${DOCKER_RUN[@]}" "$@"
fi

"${DOCKER_RUN[@]}" bash -c './build-kernel-headers.sh && make all'

cat <<'EOF'

docker-env.sh: dxgdrm.ko built. Install and load it with:

  ./docker-env.sh make install
  sudo modprobe dxgdrm
  sudo udevadm trigger --subsystem-match=drm
  sudo udevadm settle

modprobe has to run on the host -- it loads into the shared kernel. No
--force-modversion: the container's gcc 13.2.0 reproduces the kernel's CRCs,
so the module loads unforced and does not taint.
EOF
