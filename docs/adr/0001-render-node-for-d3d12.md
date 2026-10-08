# 0001. A DRM render node for the d3d12 driver, which allocates no GPU memory

Status: Accepted

## Context

WSL exposes the GPU as `/dev/dxg`, a dxgkrnl channel, and never creates a DRM
node. Mesa's d3d12 gallium driver talks to dxcore directly and does not need
one, but userspace that allocates through GBM does. Chromium's Ozone/Wayland
backend looks for a render node and treats one that cannot answer
`DRM_IOCTL_VERSION` as fatal:

    drm_render_node_handle.cc:36]  Can't get version for device: '/dev/dxg'

## Decision

- dxgdrm registers a platform device with a DRM render node. It is a path to
  open, identify and hand to `gbm_create_device()`.
- Every buffer still comes from d3d12 through `CreateSharedHandle`. The driver
  allocates no GPU memory and does not look into the buffers.
- The node also carries fences (see 0007 and 0008), and later the virtual
  display (see 0011).

## Consequences

- Chromium and other GBM users find a render node.
- The node only works together with a Mesa that knows it (see 0002). An
  unpatched Mesa gets software rendering on it.
- The module is out of tree and has to be built for each WSL kernel release
  (see 0003).
