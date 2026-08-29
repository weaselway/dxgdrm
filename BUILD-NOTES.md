# Build notes

Background for [README.md](README.md). None of this is needed to build or
install the module — it's here to explain why the build is shaped the way it
is, and to save the next person the archaeology.

## Why this needs its own kernel build

WSL ships no `/lib/modules/$(uname -r)/build`, and the running kernel is
Microsoft's, not the distro's — there is no `linux-headers` package to
install. Building this module out-of-tree means fetching the matching kernel
source and preparing it far enough to produce `Module.symvers`, since
`CONFIG_MODVERSIONS=y` means the CRCs matter.

[build-kernel-headers.sh](build-kernel-headers.sh) does that: it clones the
`microsoft/WSL2-Linux-Kernel` tag matching `uname -r`, configures it from the
in-tree `Microsoft/config-wsl`, and builds `vmlinux` + `modules_prepare` — the
minimum needed to build an out-of-tree module against it. The result is cached
under `./build/wsl-kernel` and stamped with the kernel release it was prepared
for, so it's only rebuilt after a WSL kernel update moves `uname -r`.

## Building somewhere that isn't WSL

`uname -r` is how the build finds its kernel, and on anything but WSL it points
at the wrong one — CI runners included. The configs in [conf/](conf) exist for
that case: each is a copy of the config a target kernel was built with, named
for that kernel's release, and `KERNEL_CONFIG` makes
[build-kernel-headers.sh](build-kernel-headers.sh) take both from it instead of
from the machine it is running on.

```
KERNEL_CONFIG=conf/kernel-6.18.33.2-microsoft-standard-WSL2.conf \
  ./build-kernel-headers.sh && make all
```

The release has to come from the filename because the config does not state it:
the version in it is the tree's and the `-microsoft-standard-WSL2` suffix is
`CONFIG_LOCALVERSION`, but nothing pairs them. That guess is checked rather than
trusted — the prepared tree's `include/config/kernel.release` has to match, the
same check the WSL path already made.

The GitHub Actions workflow ([.github/workflows/build.yml](.github/workflows/build.yml))
runs exactly that, through [docker-env.sh](docker-env.sh) rather than directly,
once per config in `conf/` — so adding a config there adds a build, and CI
builds in the same container everyone else does. It caches the prepared kernel tree — `vmlinux` is most of the wall clock
— keyed on the release *and* the config's hash, since editing a config leaves
the script's own `.headers-ready` stamp still looking valid.

## Matching the kernel's compiler

MODVERSIONS CRCs depend on the compiler, so a module built with the wrong one
is rejected with `disagrees about version of symbol module_layout`. It can be
forced in with `modprobe --force-modversion` — the struct layouts do match
(`CONFIG_RANDSTRUCT_NONE`, and the config differs only in compiler-capability
autodetects) — but forcing taints the kernel, so it isn't a deployment story.

The container build avoids this entirely. `/proc/version` and
`CONFIG_CC_VERSION_TEXT` both report the running kernel was built with
`gcc (GCC) 13.2.0` — bare `(GCC)`, meaning a vanilla upstream build with
`--with-pkgversion` left at its default. No distro compiler looks like that:
Fedora 39 shipped 13.2.1 stamped `(Red Hat 13.2.1-6)`, Ubuntu stamps
`(Ubuntu 13.2.0-23ubuntu4)`, and no Fedora ever shipped 13.2.0 at all. The
kernel.org crosstool builds do, so that's where the toolchain comes from.

[build-kernel-headers.sh](build-kernel-headers.sh) fetches it into
`./build/kgcc` and passes it as `CC`, and the [Makefile](Makefile) defaults
`KGCC` to that same path — so `vmlinux` and the module are built with the
kernel's own toolchain. Both have to agree: the module is checked against the
CRCs in the `Module.symvers` that the `vmlinux` build produced, so having one
place decide which compiler that is, rather than an image `ENV` on one side and
a default on the other, is the point.

It sits under `./build` next to the kernel tree because it is the same kind of
thing: a cached build input, gitignored, bind-mounted so it outlives the
container, and re-fetched when the version stamp beside it stops matching.

Only `CC` comes from that toolchain. Host programs (`genksyms` and the rest of
`scripts/`) keep using the container distro's gcc — it's a `nolibc` toolchain
and can't build them anyway, and they don't feed the CRCs, which come from the
target compiler's preprocessor output.

This is also why the container's distro is not load-bearing. It was Fedora and
is now Ubuntu 26.04, matching the other build containers in this repo; since
the compiler that determines the CRCs comes from kernel.org either way, the
module is unaffected.

This reproduces the CRCs exactly, confirmed by loading the module without
`--force-modversion`. A build that falls back to the distro compiler — i.e. one
where `./build/kgcc` isn't there or won't run — has no such luck and needs the
flag.

The other way out, independent of all this, is to boot the `vmlinux` that
`build-kernel-headers.sh` just built, via `kernel=` in `.wslconfig`.

## What the container setup is doing

It bind-mounts the repo, so `./build/wsl-kernel` persists across runs, and
nothing else — the build needs no root and writes nowhere else. What the
container does need from the host is its kernel, which it shares: `uname -r`,
and therefore `KERNEL_RELEASE` in `build-kernel-headers.sh`, is identical
inside and outside it, so what comes out is a module for the running kernel.

The container runs as `$(id -u):$(id -g)` so the kernel tree and `dxgdrm.ko`
come back owned by you rather than by root. The image has to be built for those
ids too, since `docker run --user <uid>` only resolves `HOME` and a shell if a
matching `/etc/passwd` entry exists — hence `--build-arg UID`/`GID`, and hence
the image tag carrying the uid. That account has passwordless `sudo` granted by
uid rather than by name, but only as a convenience in `./docker-env.sh bash`;
no build step uses it.

`docker-env.sh` deliberately stops after building, because the remaining step
is a `modprobe` and that has to run on the host — it loads into the kernel the
container is only borrowing. Rebuilding, on the other hand, should go back
through the container: it has to use the same gcc 13.2.0. The Makefile defaults
`KGCC` to `./build/kgcc`, so a host `make` does pick up the right compiler —
but only if that toolchain runs on the host, which is one more thing to be
right about for no gain.

Loading is a plain `modprobe` typed by hand rather than a make target, and
`--force-modversion` likewise. The Makefile builds and cleans, nothing else:
whether forcing is needed depends on what compiled the `.ko` already sitting on
disk, which is not something the build invocation knows — `KGCC` describes the
invocation it is read in, and that need not be the one that produced the module
being loaded.

## Can the module be prebuilt and shipped?

Yes. Everyone on a given WSL release runs the identical Microsoft-built kernel,
so vermagic (`UTS_RELEASE` plus a few config flags) and the struct layouts are
the same everywhere, and a container build's CRCs match. Put `dxgdrm.ko`
somewhere on the distro's own disk, `modprobe` it by that path once per boot,
and ship `99-dxgdrm.rules` alongside it.

Not `/lib/modules/$(uname -r)/extra` + `depmod -a`, which is the obvious answer
and the wrong one — see below.

Caveats:

- `uname -r` has to match exactly. A WSL kernel update moves the release string
  and the module is rejected, so a prebuilt artifact needs a rebuild per
  servicing release. Worth keying the path it is stored at on `uname -r`, so a
  stale one is a missing file rather than a vermagic error.
- x86_64 and arm64 need separate builds.
- A custom kernel via `kernel=` in `.wslconfig` is a different config and
  release, so it voids this.

## Why not /lib/modules

WSL mounts `/usr/lib/modules/$(uname -r)` itself, at boot, as an overlay:

```
none on /usr/lib/modules/6.18.33.2-microsoft-standard-WSL2 type overlay
  (lowerdir=/modules,
   upperdir=/lib/modules/6.18.33.2-microsoft-standard-WSL2/rw/upper,
   workdir=/lib/modules/6.18.33.2-microsoft-standard-WSL2/rw/work)
```

`lowerdir=/modules` is WSL's own module image, and neither it nor the `rw`
directory the upper and work layers name is reachable from inside the distro —
they exist in WSL's init mount namespace. A write into `.../extra` therefore
lands in a layer that is discarded at the next `wsl --shutdown`, taking the
`depmod` index that pointed at it along too. It looks like it worked, right up
until the reboot.

So `modprobe dxgdrm` by name cannot be the deployment story: the one directory
it searches is the one that does not keep anything. `modprobe` given a path
containing a slash loads that file directly instead, which is what both the
`modprobe ./dxgdrm.ko` in [README.md](README.md) and the weaselway repo's
`prep-session.sh` do. It skips
`modules.dep`, which costs nothing — `dxgdrm` links only against DRM core, and
`CONFIG_DRM=y`.
