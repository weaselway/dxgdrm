# dxgdrm

A DRM render node for the d3d12 Mesa driver under WSL.

WSL exposes the GPU as `/dev/dxg`, a dxgkrnl channel, and never creates a DRM
node. Mesa is fine with that — the d3d12 gallium driver talks to dxcore
directly — but userspace that allocates through GBM is not. Chromium's
Ozone/Wayland backend looks for a render node and treats one that cannot
answer `DRM_IOCTL_VERSION` as fatal. This module allocates nothing and
implements no ioctls of its own; it exists purely to be identified. See the
header comment in [dxgdrm.c](dxgdrm.c) for the full rationale, including why
dumb buffers and GEM object creation are deliberately absent, and why the
driver is named `dxgdrm` rather than something Mesa or Chromium would try to
treat specially.

[99-dxgdrm.rules](99-dxgdrm.rules) opens up permissions on the render node
(and silences a harmless `card0` permission-denied log from Mesa) as soon as
the module loads.

## Why this needs its own kernel build

WSL ships no `/lib/modules/$(uname -r)/build`, and the running kernel is
Microsoft's, not the distro's — there is no `linux-headers` package to
install. Building this module means fetching the matching kernel source and
preparing it far enough to produce `Module.symvers`, since `CONFIG_MODVERSIONS=y`
means the CRCs matter.

[build-kernel-headers.sh](build-kernel-headers.sh) does that: it clones the
`microsoft/WSL2-Linux-Kernel` tag matching `uname -r`, configures it from the
in-tree `Microsoft/config-wsl`, and builds `vmlinux` + `modules_prepare` — the
minimum needed to build an out-of-tree module against it. The result is
cached under `./build/wsl-kernel` and stamped with the kernel release it was
prepared for, so it's only rebuilt after a WSL kernel update moves `uname -r`.

## Building on the host

```sh
./build-kernel-headers.sh   # slow the first time — this is where the CRCs come from
make load                   # build dxgdrm.ko, install it, modprobe it
```

`make load` depends on `install`, which depends on `all`. Individually:

- `make all` — build `dxgdrm.ko` against `./build/wsl-kernel`
- `make install` — copy it to `/lib/modules/$(uname -r)/extra` and `depmod -a`
- `make load` — `modprobe --force-modversion dxgdrm` and trigger a udev
  re-scan so the render node appears
- `make unload` — `rmmod dxgdrm`

Host build dependencies (Fedora): `bc openssl-devel elfutils-libelf-devel
dwarves rsync flex bison`, plus the usual `git gcc make`.

### The `--force-modversion` caveat

MODVERSIONS CRCs depend on the compiler, and Fedora ships no gcc 13.2 to
match Microsoft's build, so the module needs `modprobe --force-modversion`
and taints the kernel. The struct layouts do match — `CONFIG_RANDSTRUCT_NONE`,
and the config differs only in compiler-capability autodetects — so this is
safe, but it isn't a real deployment story. The clean fix is to boot the
`vmlinux` that `build-kernel-headers.sh` just built, via `kernel=` in
`.wslconfig`.

## Building in Docker instead

If you'd rather not install kernel build tooling on the host, `docker-env.sh`
runs the same build inside a Fedora container:

```sh
./docker-env.sh              # build-kernel-headers.sh + make install, containerized
./docker-env.sh bash         # drop into the build environment instead
./docker-env.sh make load    # run any other target
```

It bind-mounts the repo (so `./build/wsl-kernel` persists across runs) and
`/lib/modules` (so `make install`'s copy + `depmod -a` land in the real host
module tree). This works because a container shares the host's kernel, so
`uname -r` — and therefore `KERNEL_RELEASE` in `build-kernel-headers.sh` —
is identical inside and outside the container.

`make load`'s `modprobe`/`udevadm trigger` still has to run on the host: it's
loading a module into the shared kernel, which isn't something to do from
inside a container. Run that step manually after `./docker-env.sh` finishes:

```sh
sudo modprobe --force-modversion dxgdrm
sudo udevadm trigger --subsystem-match=drm
sudo udevadm settle
```

## Verifying

```sh
udevadm info /dev/dri/renderD128   # DRIVERS==dxgdrm
```
