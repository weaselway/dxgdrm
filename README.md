# dxgdrm

A DRM device for the d3d12 Mesa driver under WSL: a render node, and a virtual
display for a compositor to drive.

WSL exposes the GPU as `/dev/dxg`, a dxgkrnl channel, and never creates a DRM
node. Mesa is fine with that — the d3d12 gallium driver talks to dxcore
directly — but much of userspace is not. This module does three things about
it:

- **A render node to be identified by.** Chromium's Ozone/Wayland backend
  looks for a render node and treats one that cannot answer
  `DRM_IOCTL_VERSION` as fatal. Buffers still come from d3d12; the node is a
  path to open, identify and hand to `gbm_create_device()`.
- **Real fences.** D3D12 reports completion through an eventfd, while every
  consumer on the Linux side wants a sync_file. `DXGDRM_FENCE_FROM_EVENTFD`
  turns one into the other, and the node carries `drm_syncobj`s.
- **A virtual display.** One CRTC with a primary plane (with
  `FB_DAMAGE_CLIPS`) and a cursor plane (with a hotspot), and one connector.
  An unmodified compositor, mutter's or KWin's native backend, scans out to
  it like to any other KMS device. There is no hardware behind the planes:
  each commit is handed to a userspace presenter, which reads the frame back
  and gets it to Windows. That presenter is `weaselwayd` in
  [weaselway/weaselway].

## The virtual display

The compositor uses the primary node, `card0`. The presenter's side is a
handful of ioctls on the render node, described in
[dxgdrm_drm.h](dxgdrm_drm.h):

| ioctl | What it does |
|---|---|
| `DXGDRM_GET_FRAME` | Waits for a commit and says what is on the planes: the buffer, its size and format, the damage since the last call, the cursor's image, position and hotspot, and whether a compositor owns the display. The node polls readable when there is something to fetch. |
| `DXGDRM_ACK_FRAME` | Says the presenter has taken a frame. The compositor's page flip completes only then, which holds it to the pace frames can be read back at. |
| `DXGDRM_READ_PIXELS` | Copies a dumb buffer's pixels out. |
| `DXGDRM_SET_MODE` | Sets the size of the one mode the connector offers; a hotplug event tells the compositor. |
| `DXGDRM_FENCE_FROM_EVENTFD` | The fence conversion above. |

A framebuffer is one of two things:

- **A d3d12 buffer.** It leaves Mesa as a D3D12 shared handle, an fd that is
  not a dma-buf. `PRIME_FD_TO_HANDLE` on this node accepts those and wraps the
  file, so the buffer has a GEM handle to make a framebuffer from. The
  presenter gets an fd for the same shared handle and opens it on its own
  device.
- **A dumb buffer**, for the cursor plane and for anything that draws without
  the GPU.

Mesa has to know the node: the weaselway mesa has a `dxgdrm` entry in its pipe
loader that creates the d3d12 screen. An unpatched Mesa gets software rendering
here.

Module parameters: `width` and `height` (the mode until a presenter sets one,
1920x1080), `flip_timeout_ms` (complete a page flip after this long if the
presenter has not acknowledged it, 100), and `fence_timeout_ms` (10 s).

The header comment in [dxgdrm.c](dxgdrm.c) has the full rationale, including
why the driver is named `dxgdrm` rather than something Mesa or Chromium would
try to treat specially.

[99-dxgdrm.rules](99-dxgdrm.rules) opens up permissions on both nodes as soon
as the module loads.

## Building and loading

WSL ships no `/lib/modules/$(uname -r)/build`, so building this module means
fetching the matching kernel source first. `docker-env.sh` does the whole
thing in a container and needs no build tooling on the host:

```sh
./docker-env.sh                 # build the kernel tree, then dxgdrm.ko
sudo modprobe ./dxgdrm.ko
sudo udevadm trigger --subsystem-match=drm
sudo udevadm settle
```

From `6.18.40.1` on the WSL kernel is built without DRM core. The build then
produces it as three more modules, which have to be loaded before `dxgdrm.ko`,
in this order:

```sh
sudo modprobe ./build/wsl-kernel/drivers/video/hdmi.ko
sudo modprobe ./build/wsl-kernel/drivers/gpu/drm/drm.ko
sudo modprobe ./build/wsl-kernel/drivers/gpu/drm/drm_kms_helper.ko
```

Plus the udev rules, once — `/etc/udev/rules.d` is on the distro's own disk,
so this survives a reboot and does not need repeating:

```sh
sudo install -D -m 0644 99-dxgdrm.rules /etc/udev/rules.d/99-dxgdrm.rules
sudo udevadm control --reload
```

The first run clones and builds the WSL kernel, which is slow. The result is
cached in `./build` and reused until a WSL kernel update moves `uname -r`.

There is no install step. WSL mounts `/usr/lib/modules/$(uname -r)` itself, as
an overlay whose upper layer lives in WSL's own mount namespace, so a module
copied into `.../extra` is gone again at the next `wsl --shutdown` — and
`modprobe dxgdrm` by name, which searches exactly there, then finds nothing.
The module is loaded straight out of this directory instead. The `./` matters:
`modprobe` only treats its argument as a file if it contains a slash.

The last commands run on the host: they load the module into the kernel the
container shares, which isn't something to do from inside it. Everything else
should go through `docker-env.sh`.

Requires Docker, and `sudo` on the host for the `modprobe`/`udevadm` steps.

Deploying this properly is [weaselway/weaselway]'s job: its NixOS image takes
the module from this repo's flake (see [WEASELWAY.md](WEASELWAY.md)) and loads
it once per boot from a systemd unit. This repo builds it and gets it loaded
for a look.

[weaselway/weaselway]: https://github.com/weaselway/weaselway

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

The display, with `drm_info` or `modetest` from libdrm:

```sh
modetest -M dxgdrm        # one connector, one CRTC, a primary and a cursor plane
```

## Further reading

[BUILD-NOTES.md](BUILD-NOTES.md) — why this needs its own kernel build, why
the compiler has to match the one the kernel was built with, what the
container setup is doing, and whether the module can be prebuilt and shipped
to other machines.
