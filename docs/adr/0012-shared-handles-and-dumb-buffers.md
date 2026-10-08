# 0012. Framebuffers are wrapped D3D12 shared handles or dumb buffers

Status: Accepted

## Context

A framebuffer needs a GEM handle. A d3d12 buffer leaves Mesa as a D3D12 shared
handle, an fd that is not a dma-buf, and the driver allocates no GPU memory
(see 0001). The cursor plane, and anything that draws without the GPU, needs
plain memory.

## Decision

- `PRIME_FD_TO_HANDLE` on this node accepts a D3D12 shared handle and wraps the
  file in a GEM object. The kernel never looks into it.
- `DXGDRM_GET_FRAME` gives the presenter an fd for the same shared handle,
  which it opens on its own d3d12 device.
- Dumb buffers are vmalloc memory, mmap-able, and read with
  `DXGDRM_READ_PIXELS`.
- The cursor plane takes both kinds, because KWin puts GPU buffers there
  (`c4c5d5d`).

## Consequences

- The frame never leaves GPU memory until the presenter reads it back.
- A shared handle cannot be read by the kernel or by a process without a d3d12
  device.
- Other DRM drivers cannot import these buffers. They are not dma-bufs.
