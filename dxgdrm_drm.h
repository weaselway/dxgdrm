/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UAPI for the dxgdrm render node.
 *
 * One ioctl, and it exists because of a mismatch: D3D12 completion under WSL is
 * reported through an eventfd (ID3D12Fence::SetEventOnCompletion), while every
 * consumer of a fence on the Linux side wants a sync_file backed by a
 * dma_fence. An eventfd polls the same way but fails SYNC_IOC_FILE_INFO, cannot
 * be imported into a syncobj, and cannot be merged.
 *
 * DXGDRM_FENCE_FROM_EVENTFD closes that gap: hand it an eventfd, get back a
 * sync_file whose fence signals when the eventfd does.
 */
#ifndef _DXGDRM_DRM_H_
#define _DXGDRM_DRM_H_

#ifdef __KERNEL__
#include <uapi/drm/drm.h>
#else
#include "drm.h"
#endif

#if defined(__cplusplus)
extern "C" {
#endif

struct drm_dxgdrm_fence_from_eventfd {
	/** @eventfd: eventfd to watch. Not consumed; the caller keeps it. */
	__s32 eventfd;
	/** @fd: out, a sync_file fd. */
	__s32 fd;
};

#define DRM_DXGDRM_FENCE_FROM_EVENTFD	0x00

#define DRM_IOCTL_DXGDRM_FENCE_FROM_EVENTFD				\
	DRM_IOWR(DRM_COMMAND_BASE + DRM_DXGDRM_FENCE_FROM_EVENTFD,	\
		 struct drm_dxgdrm_fence_from_eventfd)

#if defined(__cplusplus)
}
#endif

#endif /* _DXGDRM_DRM_H_ */
