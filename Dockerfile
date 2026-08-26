# Ubuntu build environment for dxgdrm, matching the rest of this repo's build
# containers (see setup/ubuntu/resolute/Dockerfile), plus the exact compiler the
# running kernel was built with -- which no distro packages. See below.
FROM ubuntu:26.04

# The kernel's own build dependencies, plus curl for the toolchain fetch below.
# Two of these are named differently than one might expect on Ubuntu:
#   pahole      generates the BTF the config asks for; Fedora splits this into
#               pahole and dwarves, Ubuntu ships one package under either name.
#   passwd      provides groupadd/useradd for the account block further down.
#               It is in the base image today; listing it keeps that from being
#               a silent assumption.
# libssl-dev and libelf-dev are openssl-devel and elfutils-libelf-devel.
RUN apt -y update \
 && apt -y install \
        git gcc make flex bison bc pahole rsync diffutils \
        openssl libssl-dev libelf-dev \
        sudo kmod passwd tar xz-utils \
        curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Note what is NOT here: the gcc 13.2.0 the running kernel was built with, and
# which the MODVERSIONS CRCs depend on. build-kernel-headers.sh fetches that
# itself, into the bind-mounted ./build alongside the kernel tree, so the two
# things that have to agree on a compiler are decided in one place and a host
# build gets the same toolchain as a container build. See the comment there.
#
# That is also why curl, ca-certificates and xz-utils are in the list above:
# they are needed at run time by that script rather than at image build time.

# The build writes into the bind-mounted repo, so it has to run as the host
# user or the tree comes back owned by root. That means baking a matching
# account into the image: docker-env.sh passes the invoking user's ids, and
# `docker run --user <uid>` resolves HOME (and a shell) out of /etc/passwd
# only if an entry with that uid exists.
#
# -o on both, because the ids are the host's and may already be taken in the
# base image -- ubuntu:26.04 ships an "ubuntu" account at uid 1000, so the
# common case collides.
#
# Passwordless sudo because `make install` writes into the bind-mounted
# /lib/modules and runs depmod -- the one step that still needs root now that
# the build itself does not. The rule is written against the uid (#N) rather
# than the name: -o allows a duplicate uid, and if the host's uid collides
# with a base-image account then getpwuid resolves to whichever /etc/passwd
# entry comes first, which need not be this one. A uid rule matches either way.
ARG UID=1000
ARG GID=1000
ARG USERNAME=builder

RUN groupadd -o -g "${GID}" "${USERNAME}" \
    && useradd -o -u "${UID}" -g "${GID}" -m -s /bin/bash "${USERNAME}" \
    && printf '#%s ALL=(ALL) NOPASSWD: ALL\n' "${UID}" \
         > "/etc/sudoers.d/99-${USERNAME}" \
    && chmod 0440 "/etc/sudoers.d/99-${USERNAME}"

RUN mkdir -p /work && chown "${UID}:${GID}" /work

USER ${UID}:${GID}
WORKDIR /work

# Deliberately no ENTRYPOINT, unlike setup/ubuntu/resolute/Dockerfile. That one
# uses `env bash` so its docker-env.sh can pass `-c '...'`; here docker-env.sh
# execs its arguments directly, which is what makes `./docker-env.sh make
# install` work. An entrypoint of bash would turn that into `bash make install`.
