# 0013. A page flip completes when the presenter has taken the frame

Status: Superseded by 0019

## Context

There is no vblank to pace the compositor. A compositor that renders faster
than the presenter reads back would fill a queue of frames, or have frames
dropped without knowing it.

## Decision

- While a presenter is attached to an output, a flip that brings a new frame
  to it completes only when the presenter acknowledges that frame for that
  output with `DXGDRM_ACK_FRAME`, or after `flip_timeout_ms` (100 by default,
  0 for not waiting) (`e96c179`). Each output is paced on its own.
- Every other commit, such as a cursor move, completes right away. The
  compositor's own frame clock limits it to the mode's refresh rate.
- Without a presenter, flips complete at once.

## Consequences

- The compositor renders at the rate frames can be read back and delivered.
  weaselwayd holds back the acknowledgement while the previous readback is
  outstanding or the client holds every buffer (weaselway 0007), so the
  client's pace reaches the compositor.
- A stuck presenter slows the compositor to one frame per `flip_timeout_ms`,
  but does not freeze it.
