# 0015. Hotspot properties without DRIVER_CURSOR_HOTSPOT, and only new frames count

Status: Accepted

## Context

The presenter wants the cursor's hotspot, so the client can make the image a
real Windows cursor. The stock way to get `HOTSPOT_X`/`HOTSPOT_Y` is the
driver feature `DRIVER_CURSOR_HOTSPOT`. It also hides the cursor plane from
every client that does not set `DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT`, and KWin
sets that only for a fixed list of VM drivers.

KWin also puts the primary plane into every atomic commit, including those
that only move the cursor. Each of those looked like a full-screen update
(`352efde`).

## Decision

- The cursor plane gets `HOTSPOT_X`/`HOTSPOT_Y` properties created by the
  driver, without the feature bit (`c4c5d5d`). mutter and KWin set them
  whenever they exist.
- The same primary framebuffer committed again without `FB_DAMAGE_CLIPS` is
  not a new frame. On the cursor plane, only a new framebuffer or explicit
  damage is a new image (`9ea59b4`).

## Consequences

- The cursor plane stays visible to every compositor, and the cursor is not
  drawn into the frame.
- Cursor moves cost no readback.
- A client that draws into the framebuffer on screen and commits it again is
  only seen if it sets `FB_DAMAGE_CLIPS`. `DRM_IOCTL_MODE_DIRTYFB`, the proper
  signal, is not implemented. Neither mutter nor KWin uses it.
