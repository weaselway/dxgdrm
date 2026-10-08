# 0008. Syncobjs come from DRM core, through feature bits only

Status: Accepted

## Context

mutter advertises `wp_linux_drm_syncobj_v1` only if
`drmGetCap(DRM_CAP_SYNCOBJ_TIMELINE)` is true and `drmSyncobjEventfd()` exists
(`meta-wayland-linux-drm-syncobj.c:521`). Every `DRM_IOCTL_SYNCOBJ_*` lives in
`drm_syncobj.c`, behind nothing but the driver's feature bits.

## Decision

The driver sets `DRIVER_SYNCOBJ` and `DRIVER_SYNCOBJ_TIMELINE` and supplies no
callbacks for them.

## Consequences

- Timeline syncobjs, sync_file import and export, and eventfd signalling work
  without driver code.
- Their fences come from `DXGDRM_FENCE_FROM_EVENTFD` (see 0007).
