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
 * sync_file whose fence signals when the eventfd does. If the eventfd is closed
 * without being signalled, the fence signals anyway; if it isn't signalled
 * within the module's fence_timeout_ms (default 10 s), the fence signals with
 * -ETIMEDOUT.
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
	/**
	 * @flags: must be 0. Added after the first release; DRM zero-extends
	 * shorter structs, so callers built against the 8-byte version keep
	 * working.
	 */
	__u32 flags;
	/** @pad: must be 0. */
	__u32 pad;
};

/*
 * The presenter side of the virtual display (kms-wsl spike).
 *
 * dxgdrm has one CRTC with a primary and a cursor plane and nothing behind
 * them. Whatever the compositor commits is handed to a userspace presenter
 * through the two ioctls below, which is what reads the frame back and gets it
 * to Windows.
 *
 * A primary framebuffer is one of two things. A d3d12 buffer is a D3D12 shared
 * handle that the compositor imported with DRM_IOCTL_PRIME_FD_TO_HANDLE (the
 * node accepts those although they are not dma-bufs); the presenter gets an fd
 * for the same shared handle and opens it on its own device. A dumb buffer is
 * plain memory and is read with DXGDRM_READ_PIXELS.
 */

#define DXGDRM_MAX_DAMAGE_RECTS		16

/* The primary plane has a framebuffer. */
#define DXGDRM_FRAME_PRIMARY		(1 << 0)
/* It is a D3D12 shared handle, and @fd is valid. */
#define DXGDRM_FRAME_SHARED		(1 << 1)
/* It is a dumb buffer, to be read with DXGDRM_READ_PIXELS. */
#define DXGDRM_FRAME_DUMB		(1 << 2)
/* Too many rects, or a new buffer layout: treat the whole frame as damaged. */
#define DXGDRM_FRAME_DAMAGE_FULL	(1 << 3)
/* The cursor plane has a framebuffer. */
#define DXGDRM_FRAME_CURSOR		(1 << 4)
/* It is a D3D12 shared handle, and @cursor_fd is valid; otherwise it is a dumb
 * buffer, to be read with DXGDRM_READ_PIXELS. */
#define DXGDRM_FRAME_CURSOR_SHARED	(1 << 5)
/*
 * A compositor controls the display (it holds DRM master). Without this flag
 * the planes may still have framebuffers: a compositor leaves its last frame
 * up when it exits, as on real hardware, so that the next one can take over
 * without a black frame in between. It is then nobody's frame, and stays until
 * someone commits again. The flag changing bumps @seq like a commit does.
 */
#define DXGDRM_FRAME_OWNED		(1 << 6)

struct drm_dxgdrm_rect {
	__s32 x1, y1, x2, y2;
};

struct drm_dxgdrm_get_frame {
	/**
	 * @seq: in, the sequence number the caller has already seen (0 at
	 * first); out, the current one. The call blocks until they differ.
	 * Every commit that touches either plane bumps it.
	 *
	 * After the first call the file also polls readable (POLLIN) while
	 * the current number differs from the one this call last returned,
	 * so a presenter can wait in poll() and then fetch without blocking.
	 */
	__u64 seq;
	/** @timeout_ms: in, give up with -ETIME after this long; 0 waits forever. */
	__u32 timeout_ms;
	/** @flags: out, DXGDRM_FRAME_*. */
	__u32 flags;

	/**
	 * @buffer_id: out, identifies the buffer behind the primary plane for
	 * as long as it exists, so the presenter can cache its import.
	 */
	__u64 buffer_id;
	/** @primary_seq: out, bumped when the primary plane's content changes. */
	__u64 primary_seq;
	/** @cursor_seq: out, bumped when the cursor image changes (not on moves). */
	__u64 cursor_seq;

	__u32 width;
	__u32 height;
	__u32 format;	/* DRM_FORMAT_* */
	__u32 pitch;

	/**
	 * @fd: out, a new fd for the shared handle if DXGDRM_FRAME_SHARED,
	 * otherwise -1. The caller closes it.
	 */
	__s32 fd;
	/** @num_damage: out, rects in @damage, accumulated since the last call. */
	__u32 num_damage;
	struct drm_dxgdrm_rect damage[DXGDRM_MAX_DAMAGE_RECTS];

	/* Cursor plane: top-left position on the CRTC and image size. */
	__s32 cursor_x;
	__s32 cursor_y;
	__u32 cursor_width;
	__u32 cursor_height;
	/**
	 * @cursor_hot_x: the point inside the image that is the pointer
	 * position, as the compositor set it through HOTSPOT_X/HOTSPOT_Y. Zero
	 * if it sets none.
	 */
	__s32 cursor_hot_x;
	__s32 cursor_hot_y;
	__u32 cursor_format;
	__u32 cursor_pitch;
	/** @cursor_buffer_id: like @buffer_id, for the cursor plane. */
	__u64 cursor_buffer_id;
	/** @cursor_fd: like @fd, if DXGDRM_FRAME_CURSOR_SHARED. */
	__s32 cursor_fd;
	__u32 pad;
};

#define DXGDRM_PLANE_PRIMARY	0
#define DXGDRM_PLANE_CURSOR	1

struct drm_dxgdrm_read_pixels {
	/** @plane: DXGDRM_PLANE_*. Its framebuffer must be a dumb buffer. */
	__u32 plane;
	/** @size: bytes available at @data. */
	__u32 size;
	/** @data: user pointer the pixels are copied to, pitch * height bytes. */
	__u64 data;
	/* out */
	__u32 width;
	__u32 height;
	__u32 format;
	__u32 pitch;
};

#define DRM_DXGDRM_FENCE_FROM_EVENTFD	0x00
#define DRM_DXGDRM_GET_FRAME		0x01
#define DRM_DXGDRM_READ_PIXELS		0x02

#define DRM_IOCTL_DXGDRM_FENCE_FROM_EVENTFD				\
	DRM_IOWR(DRM_COMMAND_BASE + DRM_DXGDRM_FENCE_FROM_EVENTFD,	\
		 struct drm_dxgdrm_fence_from_eventfd)
#define DRM_IOCTL_DXGDRM_GET_FRAME					\
	DRM_IOWR(DRM_COMMAND_BASE + DRM_DXGDRM_GET_FRAME,		\
		 struct drm_dxgdrm_get_frame)
#define DRM_IOCTL_DXGDRM_READ_PIXELS					\
	DRM_IOWR(DRM_COMMAND_BASE + DRM_DXGDRM_READ_PIXELS,		\
		 struct drm_dxgdrm_read_pixels)

#if defined(__cplusplus)
}
#endif

#endif /* _DXGDRM_DRM_H_ */
