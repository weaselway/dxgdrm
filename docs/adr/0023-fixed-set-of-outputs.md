# 0023. A fixed set of outputs, connected by the presenter

Status: Accepted

## Context

The virtual display had one CRTC and one connector, so a viewer with several
windows or monitors had to show one large desktop. That desktop was laid out
in a single buffer, its size capped and its refresh rate the same for every
monitor. A compositor already handles several outputs with their own modes,
rates and positions, if the device has them.

KMS objects are created once, when the device registers. Adding a CRTC or a
connector later is not something DRM supports for a device like this.

## Decision

- The device has `DXGDRM_MAX_OUTPUTS` (4) outputs (`cc4feac`). Each output is
  a CRTC with a primary and a cursor plane, an encoder and a connector, with
  its own mode, refresh rate, damage, flip pacing (0013) and vblank (0019,
  0020).
- Output 0 is connected from the start, at the module parameters' mode. The
  others are disconnected and report no modes until the presenter connects
  them.
- `DXGDRM_SET_MODE` connects or disconnects an output and sets its mode,
  refresh rate, physical size and the connector's suggested X and Y. A change
  sends a hotplug event, which is how monitors come and go.
- Every presenter ioctl takes the output it is about; one out of range is
  `-EINVAL`. `DXGDRM_GET_FRAME_NOWAIT` returns `-EAGAIN` when an output has
  nothing new, so a presenter waits in `poll()` and fetches every output
  without blocking.
- The presenter structs changed without compatibility, unlike 0009's rule:
  weaselwayd is their only user and moves with the driver. Driver version
  1.2.

## Consequences

- Up to four monitors, each its own buffer, size and refresh rate. A resize
  touches only its own output.
- mutter places the outputs at their suggested positions. KWin and Hyprland
  ignore those properties and lay the outputs out themselves, so the
  presenter cannot rely on its positions there.
- A weaselwayd built against an older `dxgdrm_drm.h` fails on this module,
  and the other way round. Both have to be updated together.
- A fifth monitor needs a new module build.
