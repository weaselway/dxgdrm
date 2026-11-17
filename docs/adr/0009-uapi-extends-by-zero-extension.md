# 0009. The uapi structs grow at the end, and new fields must be zero

Status: Accepted

## Context

`dxgdrm_drm.h` is the interface that weaselwayd and the weaselway Mesa build
against. `DXGDRM_FENCE_FROM_EVENTFD` was released with an 8-byte struct and no
room to extend it.

## Decision

- `flags` and `pad` were added at the end of the struct, and both must be 0
  (`c228ace`).
- DRM zero-extends a shorter ioctl struct, so callers built against the 8-byte
  version keep working.

## Consequences

- A later option can be a flag, without a new ioctl.
- The presenter ioctls are not covered by this rule. weaselwayd is their only
  user and moves with the driver, so their structs change without
  compatibility (see 0023).
