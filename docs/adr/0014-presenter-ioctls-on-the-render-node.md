# 0014. The presenter's ioctls are allowed on the render node

Status: Accepted (spike shortcut)

## Context

The presenter needs the frames, and must not hold DRM master, which the
compositor holds. The virtual display started as a spike.

## Decision

All five ioctls are `DRM_RENDER_ALLOW`, so the presenter opens the render node
like any client. The source marks this as a spike shortcut: "The real thing
wants a node or a capability of its own."

## Consequences

- The presenter needs no privileges.
- Anything that can open the render node can read the screen, set the mode,
  and acknowledge frames. With the udev rule (see 0016), that is every process
  on the system.
- Two presenters at once would steal frames and acknowledgements from each
  other.
- Fixing this needs a separate node or a capability, and a change in
  weaselwayd.
