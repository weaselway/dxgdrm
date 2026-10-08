# 0002. The driver is named dxgdrm

Status: Accepted

## Context

Userspace matches DRM drivers by name. Chromium skips a node named `vgem`
(`drm_render_node_path_finder.cc:75`), prefers i915, amdgpu and virtio_gpu,
and otherwise takes the first node left, with a warning. Mesa's pipe loader
picks the gallium driver by the name.

## Decision

- The driver is named `dxgdrm`.
- The weaselway Mesa has a `dxgdrm` entry in its pipe loader that creates the
  d3d12 screen. That screen reaches the GPU through `/dev/dxg`. It uses this
  node for fences and, on the primary node, to import scanout buffers as GEM
  handles for the compositor (mesa wsl-adr 0006 and 0008).
- `d3d12` was rejected: it would send an unpatched Mesa, and the installed
  `d3d12_dri.so`, down paths that expect a d3d12 device behind the fd.

## Consequences

- With the weaselway Mesa, gbm and EGL on this node end up on d3d12, not on
  kmsro, zink or software. Mesa matches the name before it tries
  `dri_screen_create_sw()` and kms_swrast, also for dumb buffers.
- With an unpatched Mesa, the name matches nothing and rendering falls back to
  software.
