# 0021. Wrapped shared handles can leave as dma-bufs

Status: Accepted

## Context

A D3D12 shared handle carries no fences (see 0012). A producer that hands one
to a compositor has to wait for its own GPU work first, or pass a fence over
the protocol (linux-drm-syncobj-v1). Vulkan WSI, and compositors such as
mutter, know a third way: implicit sync through the dma-buf's reservation
object (`DMA_BUF_IOCTL_IMPORT_SYNC_FILE` on the producer side, `poll()` or
`DMA_BUF_IOCTL_EXPORT_SYNC_FILE` on the compositor side). Those ioctls fail on
a shared handle, because it is not a dma-buf.

## Decision

- A shared handle imported with `PRIME_FD_TO_HANDLE` can be exported again
  with `PRIME_HANDLE_TO_FD`. That is core PRIME, unchanged: the result is a
  real dma-buf, and its reservation object is the GEM object's.
- `DXGDRM_DMABUF_TO_SHARED` gives an importer the shared handle inside such a
  dma-buf, because a D3D12 device can only open the shared handle. It takes and
  returns fds, not GEM handles, so it cannot close a handle a compositor holds
  for the same object. Any other dma-buf gets `-EINVAL`.
- The GEM objects say they have no pages (`get_sg_table` fails), so another
  device's attach fails instead of hitting a WARN in DRM core.
- The driver version is 1.1, which is how Mesa finds out the ioctl is there.

## Consequences

- dzn exports its WSI images this way. WSI's implicit sync then works, and
  mutter waits for the fences before it uses the buffer.
- Every importer of these dma-bufs has to call the ioctl first: the weaselway
  Mesa does that in `d3d12_resource_from_handle`. A Mesa without it cannot
  import them, so the module and Mesa have to be updated together.
- Only consumers that look at the reservation object wait for the fences.
  The d3d12 driver does not, so a d3d12 GL compositor that samples the buffer
  without polling it first can read an unfinished frame.
- A framebuffer made from such a dma-buf resolves to the same GEM object, so
  the presenter still finds the shared handle.
- Other DRM drivers still cannot import these buffers.
