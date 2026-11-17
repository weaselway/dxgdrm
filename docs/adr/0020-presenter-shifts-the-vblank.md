# 0020. The presenter shifts the vblank

Status: Accepted

## Context

The virtual vblank (0019) runs at the Windows monitor's rate, but in a random
phase to it, and it drifts as fast as the two clocks differ. A frame that
reaches the viewer just after the monitor's vblank waits almost a whole
refresh to be shown; one that reaches it just before misses it under any
jitter. Only the viewer can see where the frames land, and only this driver
can move the vblank.

## Decision

- `DXGDRM_SHIFT_VBLANK` moves an output's vblank by a signed number of
  nanoseconds, up to 1 s. A later call replaces what is not applied yet.
  Each output has its own phase, since each viewer window can be on its own
  monitor.
- The vblank timer's `handle_vblank_timeout` hook moves the next expiry by at
  most a quarter of a frame per vblank. The vblank core counts vblanks from
  timestamps, and a tick that moves by less than half a frame is still one
  vblank. The timestamp (expiry minus interval) moves with the expiry.
- The driver remembers the next expiry. When the vblank is turned on again
  after DRM's idle delay, the timer starts on that grid instead of one frame
  from now, so the phase survives idle periods.
- The driver knows nothing about the Windows monitor. The presenter decides
  how far to shift, from what the viewer measures.

## Consequences

- One vblank is up to a quarter of a frame longer or shorter while a shift is
  applied. mutter sees that in the presentation times and follows.
- The phase after idle is only as good as the two clocks: it drifts while the
  vblank is off, until the presenter corrects it again.
- Like the other presenter ioctls it is allowed on the render node (0014), so
  any local process can move the vblank.
