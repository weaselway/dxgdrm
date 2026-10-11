# 0018. The presenter sets the mode's refresh rate

Status: Accepted

## Context

The one mode the connector offered had a fixed 60 Hz clock. Compositors run
their frame clock at the mode's rate, so the desktop was capped at 60 fps on a
faster Windows monitor, although `DXGDRM_ACK_FRAME` already holds the
compositor to the presenter's pace (see 0013). Reported in weaselway issue #2.

## Decision

- `struct drm_dxgdrm_set_mode` gets `refresh_mhz` at the end (see 0009). 0
  means the `refresh` module parameter (Hz, default 60), which is also the
  rate until a presenter sets one.
- The rate is in millihertz, so that rates like 59.94 or 179.96 Hz survive.
  The mode's clock is computed from it; the timings stay synthetic.
- A change of the rate alone sends a hotplug event, like a change of the size.

## Consequences

- weaselwayd passes the rate the viewer reports for its monitor (weaselway
  0029). A presenter built against the old header gets the default.
- A rate higher than the presenter can deliver costs nothing: flips still wait
  for `DXGDRM_ACK_FRAME`.
- An old module ignores the field; the rate stays at 60 Hz.
