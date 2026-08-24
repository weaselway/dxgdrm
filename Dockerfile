# Fedora build environment for dxgdrm, matching what build-kernel-headers.sh
# expects on the host (see its dependency check and the comment on `install`
# in Makefile about Fedora shipping no gcc 13.2 to match Microsoft's build).
FROM fedora:44

RUN dnf install -y \
        git gcc make flex bison bc pahole rsync \
        openssl openssl-devel elfutils-libelf-devel dwarves \
        sudo \
    && dnf clean all

WORKDIR /work
