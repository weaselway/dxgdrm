# Ubuntu build environment for dxgdrm, matching the project's other build
# containers (see ubuntu/resolute/Dockerfile in the weaselway repo), plus the
# exact compiler the running kernel was built with -- which no distro packages.
# See below.
FROM ubuntu:26.04

# The kernel's own build dependencies, plus curl for the toolchain fetch below.
# Two are not the names one would guess: pahole generates the BTF the config
# asks for (Fedora splits it into pahole and dwarves), and passwd provides the
# groupadd/useradd used further down -- in the base image today, listed so that
# is not a silent assumption. libssl-dev and libelf-dev are openssl-devel and
# elfutils-libelf-devel.
RUN apt -y update \
 && apt -y install \
        git gcc make flex bison bc pahole rsync diffutils \
        openssl libssl-dev libelf-dev \
        sudo kmod passwd tar xz-utils \
        curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Note what is NOT here: the gcc 13.2.0 the running kernel was built with, and
# which the MODVERSIONS CRCs depend on. build-kernel-headers.sh fetches that
# into the bind-mounted ./build, so the compiler is decided in one place and a
# host build gets the same one as a container build. That is also why curl,
# ca-certificates and xz-utils are above -- that script needs them at run time,
# not at image build time.

# The build writes into the bind-mounted repo, so it runs as the host user or
# the tree comes back owned by root. That means baking a matching account into
# the image: `docker run --user <uid>` resolves HOME and a shell only if an
# /etc/passwd entry with that uid exists. -o on both, because the host's ids may
# already be taken -- ubuntu:26.04 ships an "ubuntu" account at uid 1000.
#
# Passwordless sudo is a convenience for `./docker-env.sh bash`; no build step
# needs root. The rule names the uid (#N) rather than the account, because with
# -o getpwuid may well resolve to the other one.
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

# Deliberately no ENTRYPOINT, unlike the weaselway repo's resolute Dockerfile.
# That one uses `env bash` so its docker-env.sh can pass `-c '...'`; here
# docker-env.sh execs its arguments directly, which is what makes
# `./docker-env.sh make all` work. An entrypoint of bash would turn that into
# `bash make all`.
