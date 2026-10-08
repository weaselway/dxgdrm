# 0016. The udev rule sets mode 0666 on both nodes, and no group

Status: Accepted

## Context

The stock rule gives render nodes their permissions only at the next udev
trigger. Which group owns render nodes varies by distribution (`render`,
`video`, or none inside a WSL rootfs), and udev drops a whole rule whose group
does not resolve. Mesa opens the primary node while enumerating devices, and
logs `Opening /dev/dri/card0 failed: Permission denied` on every client start
without access.

## Decision

`99-dxgdrm.rules` sets `MODE="0666"` on the render node and the primary node of
dxgdrm, and no `GROUP`.

## Consequences

- Both nodes are usable as soon as the module loads, on any distribution.
- Every local process can open both nodes. Modesetting still needs DRM master,
  which logind gives the session, but the presenter's ioctls do not (see
  0014).
- The rule is not always applied in time. weaselway adds the user to `render`
  and `video` to cover that.
