# 0011. A virtual KMS display whose commits go to a userspace presenter

Status: Accepted, the vblank superseded by 0019

## Context

weaselway served the desktop from an RDP backend in a forked mutter, which
limited it to GNOME. Every Wayland compositor has a KMS backend. If the node
were also a display, an unmodified compositor could scan out to it, and a
separate process could get the frames to Windows.

## Decision

- The device has `DXGDRM_MAX_OUTPUTS` (4) outputs (see 0023). Each is a CRTC
  with a primary plane (XRGB8888 and ARGB8888, with `FB_DAMAGE_CLIPS`), a
  256 x 256 cursor plane, an encoder and a connector (`7909f33`, `cc4feac`).
  The compositor uses the primary node, `card0`.
- Nothing is scanned out and there is no vblank. A commit records which
  framebuffer each plane of an output shows and what was damaged, bumps that
  output's sequence number and wakes the presenter.
- The presenter, weaselwayd, uses ioctls on the render node (see 0014). Each
  takes the output it is about:
  - `DXGDRM_GET_FRAME` returns the buffer, its size and format, the damage
    since the last call, the cursor's image, position and hotspot, and whether
    a compositor holds DRM master. After the first call the file polls
    readable when there is a commit to fetch (`a923e2a`), and
    `DXGDRM_GET_FRAME_NOWAIT` fetches an output without blocking.
  - `DXGDRM_ACK_FRAME` paces the compositor (see 0013).
  - `DXGDRM_READ_PIXELS` copies a dumb buffer's pixels out.
  - `DXGDRM_SET_MODE` connects or disconnects an output, and sets the size of
    the one mode its connector offers, its suggested position and its
    physical size. If anything changed it sends a hotplug event, so the
    screen follows the viewer's window (`e96c179`).
- Module parameters `width` and `height` set output 0's mode until a
  presenter does (1920 x 1080).

## Consequences

- mutter, KWin and other compositors run on it unchanged.
- The kernel holds no pixels. Reading the frame back is the presenter's job.
- Up to four monitors, one per output, each with its own size and rate.
- Damage is up to `DXGDRM_MAX_DAMAGE_RECTS` rectangles, beyond which the frame
  counts as fully damaged.
