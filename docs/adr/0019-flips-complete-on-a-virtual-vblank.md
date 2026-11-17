# 0019. Flips complete on a virtual vblank

Status: Accepted

## Context

A flip completed when the presenter acknowledged the frame (see 0013), and
every other commit at once. mutter takes the flip time as the presentation
time and plans the next frame one refresh interval after it. The
acknowledgement comes at a random point, so the desktop ran at 63-75 fps on a
60 Hz monitor, in no fixed phase to it. The viewer's vsync then showed some
frames twice and dropped others (3-15 %).

mutter's KMS deadline timer also calls `DRM_IOCTL_WAIT_VBLANK`. Without
vblank support that fails with `EPERM`, and mutter turns the timer off without
a warning.

## Decision

- Each CRTC has a vblank: `drm_vblank_init()` and DRM's vblank timer
  (`DRM_CRTC_VBLANK_TIMER_FUNCS`), an hrtimer at the rate of its output's
  mode, which the presenter sets (see 0018). It runs while someone holds a
  vblank reference, and stops after DRM's idle delay.
- Every flip completes on a vblank, with that vblank's count and time:
  - a flip that brings a new frame on the first vblank after the presenter has
    acknowledged it, or after `flip_timeout_ms`;
  - any other flip, such as a cursor move, on the next vblank.
- With the CRTC off, a flip completes at once.

## Consequences

- The compositor sees presentation times on a regular grid and runs at the
  mode's rate. On the HD 630 at 1440p60, with a light client, the guest and
  the viewer both showed 60.0 fps, and the viewer dropped no frames.
- The phase to the Windows monitor's vblank is still random, and it drifts as
  fast as the two clocks differ.
- mutter commits about 1.3 ms before the vblank. The acknowledgement has to
  come within that, so weaselwayd acknowledges before it issues the readback
  (weaselway 0007). Otherwise half of the frames miss their vblank, and the
  desktop runs at 35-45 fps.
- `DRM_IOCTL_WAIT_VBLANK` works, and mutter's deadline timer stays on.
- Cursor moves are limited to the refresh rate, as on real hardware.
- It needs DRM's vblank timer helpers (`drm_vblank_helper.h`), which
  `6.18.33.2` does not have, so that kernel is no longer supported (0006).
