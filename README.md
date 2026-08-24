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

## Installing

WSL ships no `/lib/modules/$(uname -r)/build`, so building this module means
fetching the matching kernel source first. `docker-env.sh` does the whole
thing in a container and needs no build tooling on the host:

```sh
./docker-env.sh                 # build the kernel tree, then dxgdrm.ko
./docker-env.sh make install    # copy into /lib/modules/$(uname -r)/extra
sudo modprobe dxgdrm
sudo udevadm trigger --subsystem-match=drm
sudo udevadm settle
```

The first run clones and builds the WSL kernel, which is slow. The result is
cached in `./build` and reused until a WSL kernel update moves `uname -r`.

The last three commands run on the host: they load the module into the kernel
the container shares, which isn't something to do from inside it. Everything
else, `make install` included, should go through `docker-env.sh`.

Requires Docker, and `sudo` on the host for the `modprobe`/`udevadm` steps.

## Verifying

```sh
basename "$(readlink -f /sys/class/drm/renderD128/device/driver)"  # dxgdrm render
```

The render node hangs off a platform device the module registers, so its
driver symlink is what identifies it. To check the same thing the way the udev
rules match on it:

```sh
udevadm info -a /dev/dri/renderD128 | grep DRIVERS   # DRIVERS=="dxgdrm"
```

## Further reading

[BUILD-NOTES.md](BUILD-NOTES.md) — why this needs its own kernel build, why
the compiler has to match the one the kernel was built with, what the
container setup is doing, and whether the module can be prebuilt and shipped
to other machines.
