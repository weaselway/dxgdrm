# Fedora build environment for dxgdrm, matching what build-kernel-headers.sh
# expects on the host (see its dependency check), plus the exact compiler the
# running kernel was built with -- which no distro packages. See below.
FROM fedora:44

RUN dnf install -y \
        git gcc make flex bison bc pahole rsync diffutils \
        openssl openssl-devel elfutils-libelf-devel dwarves \
        sudo kmod shadow-utils tar xz \
    && dnf clean all

# MODVERSIONS CRCs depend on the compiler, so a module built with a different
# one is rejected with "disagrees about version of symbol module_layout" and
# needs modprobe --force-modversion, which taints the kernel.
#
# /proc/version and CONFIG_CC_VERSION_TEXT both report the running kernel was
# built with "gcc (GCC) 13.2.0" -- bare "(GCC)", i.e. a vanilla upstream build
# with --with-pkgversion left at its default. No distro compiler looks like
# that: Fedora 39 shipped 13.2.1 stamped "(Red Hat 13.2.1-6)", Ubuntu stamps
# "(Ubuntu 13.2.0-23ubuntu4)", and no Fedora ever shipped 13.2.0 at all. The
# kernel.org crosstool builds do, which is why the toolchain comes from there.
#
# Only CC comes from this toolchain. Host programs (genksyms and the rest of
# scripts/) keep using Fedora's gcc: this is a nolibc toolchain and cannot
# build them anyway, and they do not feed the CRCs, which come from the target
# compiler's preprocessor output.
#
# Placed before the ARG UID block on purpose -- an ARG invalidates every layer
# after it, and this download should not be repeated for each uid.
ARG KGCC_URL=https://mirrors.edge.kernel.org/pub/tools/crosstool/files/bin/x86_64/13.2.0/x86_64-gcc-13.2.0-nolibc-x86_64-linux.tar.xz
ENV KGCC=/opt/kgcc/bin/x86_64-linux-gcc

# curl is not in the dnf list above: the base image already has curl-minimal,
# which provides the binary, and asking for curl proper conflicts with it.
#
# --strip-components=2 drops the archive's gcc-13.2.0-nolibc/x86_64-linux/
# prefix; gcc locates its own libexec relative to the binary, so relocating the
# tree is fine. Running --version is the check that the layout was as expected:
# if it was not, the image build fails here rather than the kernel build later.
RUN mkdir -p /opt/kgcc \
    && curl -fsSL "${KGCC_URL}" | tar -xJ -C /opt/kgcc --strip-components=2 \
    && "${KGCC}" --version

# The build writes into the bind-mounted repo, so it has to run as the host
# user or the tree comes back owned by root. That means baking a matching
# account into the image: docker-env.sh passes the invoking user's ids, and
# `docker run --user <uid>` resolves HOME (and a shell) out of /etc/passwd
# only if an entry with that uid exists.
#
# -o on both, because the ids are the host's and may already be taken in the
# base image.
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
