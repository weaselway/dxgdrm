// SPDX-License-Identifier: GPL-2.0
/*
 * dxgdrm - a DRM render node for the d3d12 Mesa driver under WSL.
 *
 * WSL exposes the GPU as /dev/dxg, a dxgkrnl channel, and never creates a DRM
 * node. Mesa is fine with that -- the d3d12 gallium driver talks to dxcore --
 * but userspace that allocates through GBM is not. Chromium's Ozone/Wayland
 * backend looks for a render node, and treats a node that cannot answer
 * DRM_IOCTL_VERSION as fatal:
 *
 *     drm_render_node_handle.cc:36]  Can't get version for device: '/dev/dxg'
 *
 * That was the original requirement: Chromium wants a path it can open,
 * identify and hand to gbm_create_device(). Every buffer still comes from
 * d3d12 through CreateSharedHandle, so this driver allocates nothing. It has
 * since picked up two more jobs: carrying drm_syncobjs (see the driver
 * features below) and turning the eventfd behind a d3d12 fence into a real
 * sync_file (DXGDRM_FENCE_FROM_EVENTFD).
 *
 * kms-wsl spike: the node is also a virtual display. It has one CRTC with a
 * primary and a cursor plane, so an unmodified compositor (mutter's or KWin's
 * native backend) can scan out to it like to any other KMS device. There is no
 * hardware behind the planes: each commit is handed to a userspace presenter
 * (DXGDRM_GET_FRAME), which reads the frame back and gets it to Windows. See
 * "Virtual display" below.
 *
 * That needs two kinds of GEM object, neither of which the kernel ever looks
 * into:
 *
 *   - Shared handles. A d3d12 buffer leaves Mesa as a D3D12 shared handle, an
 *     fd that is not a dma-buf. PRIME_FD_TO_HANDLE accepts those here and wraps
 *     the file, so the buffer has a GEM handle to make a framebuffer from. The
 *     presenter gets the same file back and opens it on its own device.
 *
 *   - Dumb buffers, for the cursor plane (and for anything that draws without
 *     the GPU). Mesa still ends up on d3d12 for this node: its pipe loader
 *     matches the driver name before dri_screen_create_sw() and kms_swrast are
 *     ever tried. An unpatched Mesa would get software rendering here.
 *
 * On the name. It must not be "vgem" -- Chromium skips that node by name
 * (drm_render_node_path_finder.cc:75) -- and beyond that Chromium does not care;
 * it prefers i915/amdgpu/virtio_gpu and otherwise takes the first survivor with
 * a warning. Mesa cares more: the name is what its pipe loader matches drivers
 * on. The weaselway mesa has a "dxgdrm" entry there that creates the d3d12
 * screen (which reaches the GPU through /dev/dxg and uses this node only for
 * fences), so gbm and EGL on this node end up on d3d12 rather than falling
 * back to kmsro, zink or software. "d3d12" itself would be wrong: that name
 * would send an unpatched mesa, and the installed d3d12_dri.so, down paths
 * that expect a d3d12 device behind the fd.
 */

#include <linux/dma-fence.h>
#include <linux/eventfd.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/irq_work.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/workqueue.h>

#include <linux/dma-buf.h>
#include <linux/mm.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_prime.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vma_manager.h>

#include "dxgdrm_drm.h"

#define DRIVER_NAME	"dxgdrm"
#define DRIVER_DESC	"Render node for the WSL d3d12 Mesa driver"

struct dxgdrm_device {
	struct drm_device base;

	struct drm_plane primary;
	struct drm_plane cursor;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	/* What the presenter is told, see dxgdrm_get_frame_ioctl(). */
	struct mutex frame_lock;
	wait_queue_head_t frame_wq;
	u64 seq;
	u64 primary_seq;
	u64 cursor_seq;
	struct drm_framebuffer *primary_fb;
	struct drm_framebuffer *cursor_fb;
	int cursor_x, cursor_y;
	int cursor_hot_x, cursor_hot_y;
	struct drm_dxgdrm_rect damage[DXGDRM_MAX_DAMAGE_RECTS];
	unsigned int num_damage;
	bool damage_full;
};

#define to_dxgdrm(dev) container_of(dev, struct dxgdrm_device, base)

/*
 * A dma_fence that signals when an eventfd does.
 *
 * D3D12 reports completion by signalling an eventfd, and that is all the signal
 * we have: there is no dma_fence anywhere in the WSL GPU stack to borrow. But
 * everything downstream wants one -- drm_syncobj will only import a sync_file,
 * and Chromium's explicit-sync path discards the frame outright when the import
 * fails (wayland_surface.cc:444), so "poll works on it" is not good enough.
 *
 * The mechanism is the one KVM's irqfd uses: attach a wait queue entry to the
 * eventfd via vfs_poll() and signal the fence from the wakeup. Nothing here
 * reads the eventfd -- a read would consume the count that Mesa's own waiter is
 * looking for -- so this only ever observes.
 *
 * The caller keeps its eventfd. This holds only the eventfd_ctx (which owns the
 * wait queue), not the file, so closing the last file reference still wakes us
 * with EPOLLHUP.
 *
 * Each fence gets its own fence context. They are not ordered with respect to
 * each other, and sync_file_merge()/dma_resv keep only the newest fence per
 * context, so a shared context would let a merged fence signal early.
 *
 * The fence is never signalled from inside the eventfd wakeup: that runs with
 * current->in_eventfd set, and a drm_syncobj eventfd chained off this fence
 * would then hit the recursion check in eventfd_signal() and be dropped. The
 * wakeup queues an irq_work instead.
 */

static unsigned int fence_timeout_ms = 10000;
module_param(fence_timeout_ms, uint, 0644);
MODULE_PARM_DESC(fence_timeout_ms,
		 "Signal an exported fence with -ETIMEDOUT if its eventfd has not fired after this many ms (0 = never)");

static struct workqueue_struct *dxgdrm_wq;

static const char *dxgdrm_fence_get_name(struct dma_fence *fence)
{
	return "dxgdrm";
}

static void dxgdrm_fence_release_work(struct work_struct *work);

struct dxgdrm_fence {
	struct dma_fence	base;
	spinlock_t		lock;
	struct eventfd_ctx	*ctx;
	wait_queue_head_t	*wqh;
	wait_queue_entry_t	wait;
	poll_table		pt;
	struct irq_work		signal_work;
	struct delayed_work	watchdog;
	struct work_struct	release_work;
};

static void dxgdrm_fence_release(struct dma_fence *fence)
{
	struct dxgdrm_fence *f = container_of(fence, struct dxgdrm_fence, base);

	/* remove_wait_queue(), the *_sync() calls and eventfd_ctx_put() can
	 * all sleep, and a fence may be put from atomic context. */
	INIT_WORK(&f->release_work, dxgdrm_fence_release_work);
	queue_work(dxgdrm_wq, &f->release_work);
}

static void dxgdrm_fence_release_work(struct work_struct *work)
{
	struct dxgdrm_fence *f =
		container_of(work, struct dxgdrm_fence, release_work);

	/* Unhook first so the wakeup can't queue signal_work again. */
	if (f->wqh)
		remove_wait_queue(f->wqh, &f->wait);
	irq_work_sync(&f->signal_work);
	cancel_delayed_work_sync(&f->watchdog);

	eventfd_ctx_put(f->ctx);

	/* drm_syncobj and dma_resv look at fences under RCU. */
	kfree_rcu(f, base.rcu);

	/* dxgdrm_exit() drains dxgdrm_wq, so this function finishes before
	 * the module text goes away. */
	module_put(THIS_MODULE);
}

static const struct dma_fence_ops dxgdrm_fence_ops = {
	.get_driver_name	= dxgdrm_fence_get_name,
	.get_timeline_name	= dxgdrm_fence_get_name,
	.release		= dxgdrm_fence_release,
};

static void dxgdrm_fence_signal_work(struct irq_work *work)
{
	struct dxgdrm_fence *f =
		container_of(work, struct dxgdrm_fence, signal_work);

	dma_fence_signal(&f->base);
	cancel_delayed_work(&f->watchdog);
}

static void dxgdrm_fence_watchdog(struct work_struct *work)
{
	struct dxgdrm_fence *f =
		container_of(to_delayed_work(work), struct dxgdrm_fence, watchdog);
	unsigned long flags;

	/* A fence that never signals hangs whoever waits on it. Better to
	 * signal it with an error, which consumers can at least see. */
	spin_lock_irqsave(&f->lock, flags);
	if (!dma_fence_is_signaled_locked(&f->base)) {
		dma_fence_set_error(&f->base, -ETIMEDOUT);
		dma_fence_signal_locked(&f->base);
	}
	spin_unlock_irqrestore(&f->lock, flags);
}

static int dxgdrm_fence_wakeup(wait_queue_entry_t *wait, unsigned int mode,
			       int sync, void *key)
{
	struct dxgdrm_fence *f = container_of(wait, struct dxgdrm_fence, wait);
	__poll_t flags = key_to_poll(key);

	/* EPOLLHUP means the eventfd was closed without ever being signalled.
	 * Signalling anyway is the only safe move: a fence that never signals
	 * hangs whoever waits on it. */
	if (flags & (EPOLLIN | EPOLLHUP))
		irq_work_queue(&f->signal_work);

	return 0;
}

static void dxgdrm_fence_queue_proc(struct file *file, wait_queue_head_t *wqh,
				    poll_table *pt)
{
	struct dxgdrm_fence *f = container_of(pt, struct dxgdrm_fence, pt);

	f->wqh = wqh;
	add_wait_queue(wqh, &f->wait);
}

static int dxgdrm_fence_from_eventfd(struct drm_device *dev, void *data,
				     struct drm_file *file)
{
	struct drm_dxgdrm_fence_from_eventfd *args = data;
	struct dxgdrm_fence *f;
	struct eventfd_ctx *ctx;
	struct sync_file *sync_file;
	struct file *efile;
	__poll_t events;
	int fd, ret;

	if (args->flags || args->pad)
		return -EINVAL;

	/* One lookup for both the ctx and the poll, so the fd can't be swapped
	 * for another file in between. */
	efile = fget(args->eventfd);
	if (!efile)
		return -EBADF;

	/* Reject anything that is not an eventfd up front: vfs_poll() would
	 * happily watch some other pollable fd and produce a fence that signals
	 * on unrelated readability. */
	ctx = eventfd_ctx_fileget(efile);
	if (IS_ERR(ctx)) {
		fput(efile);
		return PTR_ERR(ctx);
	}

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		eventfd_ctx_put(ctx);
		fput(efile);
		return -ENOMEM;
	}

	/* Dropped by dxgdrm_fence_release_work(). Can't fail: we are running
	 * this module's ioctl. */
	__module_get(THIS_MODULE);

	f->ctx = ctx;
	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &dxgdrm_fence_ops, &f->lock,
		       dma_fence_context_alloc(1), 1);

	init_irq_work(&f->signal_work, dxgdrm_fence_signal_work);
	INIT_DELAYED_WORK(&f->watchdog, dxgdrm_fence_watchdog);
	init_waitqueue_func_entry(&f->wait, dxgdrm_fence_wakeup);
	init_poll_funcptr(&f->pt, dxgdrm_fence_queue_proc);

	/* Queues f->wait on the eventfd's wait queue as a side effect, and
	 * reports whether it is signalled already. The wait queue lives in
	 * the ctx, so the file reference isn't needed after this. */
	events = vfs_poll(efile, &f->pt);
	fput(efile);

	sync_file = sync_file_create(&f->base);
	if (!sync_file) {
		ret = -ENOMEM;
		goto err_put_fence;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto err_put_file;
	}

	/* Only now that nothing can fail: if the eventfd was already signalled
	 * the wakeup never comes, so settle the fence by hand. */
	if (events & EPOLLIN)
		dma_fence_signal(&f->base);
	else if (fence_timeout_ms)
		queue_delayed_work(dxgdrm_wq, &f->watchdog,
				   msecs_to_jiffies(fence_timeout_ms));

	fd_install(fd, sync_file->file);
	dma_fence_put(&f->base);

	args->fd = fd;
	return 0;

err_put_file:
	fput(sync_file->file);
err_put_fence:
	dma_fence_put(&f->base);
	return ret;
}

/*
 * GEM objects.
 *
 * Either a wrapped D3D12 shared handle (@shared) or a dumb buffer (@vaddr),
 * never both.
 */

/* A shared handle has no size the kernel could know. The framebuffer helper
 * checks the object against the framebuffer's size, so claim one that is
 * large enough for any mode this device accepts. Nothing maps these. */
#define DXGDRM_SHARED_SIZE	SZ_1G
#define DXGDRM_DUMB_MAX_SIZE	SZ_256M

struct dxgdrm_gem {
	struct drm_gem_object base;
	struct file *shared;
	void *vaddr;
	/* For the presenter's import cache. */
	u64 id;
};

#define to_dxgdrm_gem(obj) container_of(obj, struct dxgdrm_gem, base)

static atomic64_t dxgdrm_buffer_ids;

static void dxgdrm_gem_free(struct drm_gem_object *obj)
{
	struct dxgdrm_gem *gem = to_dxgdrm_gem(obj);

	if (gem->shared)
		fput(gem->shared);
	vfree(gem->vaddr);
	drm_gem_object_release(obj);
	kfree(gem);
}

static vm_fault_t dxgdrm_gem_fault(struct vm_fault *vmf)
{
	struct dxgdrm_gem *gem = to_dxgdrm_gem(vmf->vma->vm_private_data);
	struct page *page;

	if (vmf->pgoff >= gem->base.size >> PAGE_SHIFT)
		return VM_FAULT_SIGBUS;

	page = vmalloc_to_page(gem->vaddr + (vmf->pgoff << PAGE_SHIFT));
	if (!page)
		return VM_FAULT_SIGBUS;

	get_page(page);
	vmf->page = page;
	return 0;
}

static const struct vm_operations_struct dxgdrm_gem_vm_ops = {
	.fault	= dxgdrm_gem_fault,
	.open	= drm_gem_vm_open,
	.close	= drm_gem_vm_close,
};

static int dxgdrm_gem_mmap(struct drm_gem_object *obj,
			   struct vm_area_struct *vma)
{
	struct dxgdrm_gem *gem = to_dxgdrm_gem(obj);

	if (!gem->vaddr)
		return -ENODEV;

	/* Drop the fake offset, so the fault handler sees page indices. */
	vma->vm_pgoff -= drm_vma_node_start(&obj->vma_node);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	return 0;
}

static const struct drm_gem_object_funcs dxgdrm_gem_funcs = {
	.free	= dxgdrm_gem_free,
	.mmap	= dxgdrm_gem_mmap,
	.vm_ops	= &dxgdrm_gem_vm_ops,
};

static struct dxgdrm_gem *dxgdrm_gem_create(struct drm_device *dev, size_t size)
{
	struct dxgdrm_gem *gem;

	gem = kzalloc(sizeof(*gem), GFP_KERNEL);
	if (!gem)
		return NULL;

	gem->base.funcs = &dxgdrm_gem_funcs;
	gem->id = atomic64_inc_return(&dxgdrm_buffer_ids);
	drm_gem_private_object_init(dev, &gem->base, size);
	return gem;
}

static int dxgdrm_dumb_create(struct drm_file *file, struct drm_device *dev,
			      struct drm_mode_create_dumb *args)
{
	struct dxgdrm_gem *gem;
	u64 pitch, size;
	int ret;

	pitch = DIV_ROUND_UP((u64)args->width * args->bpp, 8);
	size = PAGE_ALIGN(pitch * args->height);
	if (!size || size > DXGDRM_DUMB_MAX_SIZE)
		return -EINVAL;

	gem = dxgdrm_gem_create(dev, size);
	if (!gem)
		return -ENOMEM;

	gem->vaddr = vzalloc(size);
	if (!gem->vaddr) {
		ret = -ENOMEM;
		goto out;
	}

	ret = drm_gem_handle_create(file, &gem->base, &args->handle);
	if (!ret) {
		args->pitch = pitch;
		args->size = size;
	}
out:
	/* The handle holds the reference now, or nothing does. */
	drm_gem_object_put(&gem->base);
	return ret;
}

/*
 * PRIME import that also takes D3D12 shared handles.
 *
 * A compositor makes a framebuffer by exporting its buffer and importing the
 * fd here (KWin does exactly that; mutter asks gbm for the handle, and Mesa
 * does the import on its behalf). With d3d12 that fd is a shared handle from
 * dxgkrnl, which dma_buf_get() rejects. There is nothing in the file we could
 * check either -- dxgkrnl exports no way to identify its files -- so any fd
 * that is not a dma-buf is taken on trust and merely kept alive. The presenter
 * finds out whether it is one when it tries to open it.
 *
 * Every import makes a new object and handle. That is deliberate: a compositor
 * closes the handle once it has its framebuffer, and with core PRIME's
 * one-handle-per-buffer rule that would close Mesa's handle for the same
 * buffer along with it.
 */
static int dxgdrm_prime_fd_to_handle(struct drm_device *dev,
				     struct drm_file *file_priv, int prime_fd,
				     uint32_t *handle)
{
	struct dma_buf *dmabuf;
	struct dxgdrm_gem *gem;
	struct file *shared;
	int ret;

	dmabuf = dma_buf_get(prime_fd);
	if (!IS_ERR(dmabuf)) {
		dma_buf_put(dmabuf);
		return drm_gem_prime_fd_to_handle(dev, file_priv, prime_fd,
						  handle);
	}

	shared = fget(prime_fd);
	if (!shared)
		return -EBADF;

	gem = dxgdrm_gem_create(dev, DXGDRM_SHARED_SIZE);
	if (!gem) {
		fput(shared);
		return -ENOMEM;
	}
	gem->shared = shared;

	ret = drm_gem_handle_create(file_priv, &gem->base, handle);
	drm_gem_object_put(&gem->base);
	return ret;
}

/*
 * Virtual display.
 *
 * Nothing is scanned out. A commit records which framebuffer each plane shows
 * and what was damaged, bumps a sequence number and wakes the presenter. There
 * is no vblank either: the atomic helpers complete every flip right away
 * (drm_atomic_helper_fake_vblank()), and the compositor's own frame clock is
 * what limits it to the mode's refresh rate.
 */

static unsigned int width = 1920;
module_param(width, uint, 0444);
MODULE_PARM_DESC(width, "Width of the virtual display's preferred mode");

static unsigned int height = 1080;
module_param(height, uint, 0444);
MODULE_PARM_DESC(height, "Height of the virtual display's preferred mode");

#define DXGDRM_MAX_DIMENSION	8192
#define DXGDRM_CURSOR_SIZE	256

static const u32 dxgdrm_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const u32 dxgdrm_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

static int dxgdrm_plane_atomic_check(struct drm_plane *plane,
				     struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	bool is_cursor = plane->type == DRM_PLANE_TYPE_CURSOR;
	struct drm_crtc_state *crtc_state;

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   is_cursor, true);
}

static void dxgdrm_add_damage(struct dxgdrm_device *dxg,
			      const struct drm_rect *clip)
{
	struct drm_dxgdrm_rect *rect;

	if (dxg->damage_full)
		return;

	if (dxg->num_damage == DXGDRM_MAX_DAMAGE_RECTS) {
		dxg->damage_full = true;
		return;
	}

	rect = &dxg->damage[dxg->num_damage++];
	rect->x1 = clip->x1;
	rect->y1 = clip->y1;
	rect->x2 = clip->x2;
	rect->y2 = clip->y2;
}

static void dxgdrm_primary_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct dxgdrm_device *dxg = to_dxgdrm(plane->dev);
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_framebuffer *old_fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;

	if (fb)
		drm_framebuffer_get(fb);

	mutex_lock(&dxg->frame_lock);
	old_fb = dxg->primary_fb;
	dxg->primary_fb = fb;

	/* The presenter keeps one copy of the screen across all the buffers a
	 * compositor flips between, so what it needs is the damage since the
	 * previous frame -- which is what FB_DAMAGE_CLIPS carries. Without the
	 * property the iterator yields the whole plane. A different size or
	 * format invalidates the presenter's copy altogether. */
	if (!fb || !old_fb || fb->width != old_fb->width ||
	    fb->height != old_fb->height || fb->format != old_fb->format) {
		dxg->damage_full = true;
	} else {
		drm_atomic_helper_damage_iter_init(&iter, old_state, new_state);
		drm_atomic_for_each_plane_damage(&iter, &clip)
			dxgdrm_add_damage(dxg, &clip);
	}

	dxg->primary_seq++;
	dxg->seq++;
	mutex_unlock(&dxg->frame_lock);

	if (old_fb)
		drm_framebuffer_put(old_fb);
	wake_up_interruptible_all(&dxg->frame_wq);
}

static void dxgdrm_cursor_atomic_update(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct dxgdrm_device *dxg = to_dxgdrm(plane->dev);
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_framebuffer *old_fb;
	struct drm_rect damage;

	if (fb)
		drm_framebuffer_get(fb);

	mutex_lock(&dxg->frame_lock);
	old_fb = dxg->cursor_fb;
	dxg->cursor_fb = fb;
	dxg->cursor_x = new_state->crtc_x;
	dxg->cursor_y = new_state->crtc_y;
	dxg->cursor_hot_x = new_state->hotspot_x;
	dxg->cursor_hot_y = new_state->hotspot_y;

	/* A move alone leaves the image as it was. */
	if (fb != old_fb ||
	    drm_atomic_helper_damage_merged(old_state, new_state, &damage))
		dxg->cursor_seq++;
	dxg->seq++;
	mutex_unlock(&dxg->frame_lock);

	if (old_fb)
		drm_framebuffer_put(old_fb);
	wake_up_interruptible_all(&dxg->frame_wq);
}

static const struct drm_plane_helper_funcs dxgdrm_primary_helper_funcs = {
	.prepare_fb	= drm_gem_plane_helper_prepare_fb,
	.atomic_check	= dxgdrm_plane_atomic_check,
	.atomic_update	= dxgdrm_primary_atomic_update,
};

static const struct drm_plane_helper_funcs dxgdrm_cursor_helper_funcs = {
	.prepare_fb	= drm_gem_plane_helper_prepare_fb,
	.atomic_check	= dxgdrm_plane_atomic_check,
	.atomic_update	= dxgdrm_cursor_atomic_update,
};

static const struct drm_plane_funcs dxgdrm_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

/*
 * HOTSPOT_X/HOTSPOT_Y on the cursor plane, without DRIVER_CURSOR_HOTSPOT.
 *
 * The presenter wants the hotspot so the client can make the image a real
 * Windows cursor. The stock way to get the properties is the driver feature,
 * but that also hides the cursor plane from every client that does not set
 * DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT -- and KWin only sets it for a fixed
 * list of VM drivers, so it would lose the plane and draw the cursor into the
 * frame. Without the feature bit the plane stays visible to everyone, and both
 * mutter and KWin set the properties whenever they exist. drm core stores them
 * in the plane state once the plane's property pointers are set.
 */
static int dxgdrm_cursor_create_hotspot_properties(struct drm_plane *plane)
{
	struct drm_property *prop_x, *prop_y;

	prop_x = drm_property_create_signed_range(plane->dev, 0, "HOTSPOT_X",
						  INT_MIN, INT_MAX);
	prop_y = drm_property_create_signed_range(plane->dev, 0, "HOTSPOT_Y",
						  INT_MIN, INT_MAX);
	/* Properties are freed with the mode config. */
	if (!prop_x || !prop_y)
		return -ENOMEM;

	drm_object_attach_property(&plane->base, prop_x, 0);
	drm_object_attach_property(&plane->base, prop_y, 0);
	plane->hotspot_x_property = prop_x;
	plane->hotspot_y_property = prop_y;
	return 0;
}

static void dxgdrm_crtc_atomic_enable(struct drm_crtc *crtc,
				      struct drm_atomic_state *state)
{
}

static void dxgdrm_crtc_atomic_disable(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
}

static const struct drm_crtc_helper_funcs dxgdrm_crtc_helper_funcs = {
	.atomic_enable	= dxgdrm_crtc_atomic_enable,
	.atomic_disable	= dxgdrm_crtc_atomic_disable,
};

static const struct drm_crtc_funcs dxgdrm_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.destroy		= drm_crtc_cleanup,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
};

static int dxgdrm_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	int count;

	count = drm_add_modes_noedid(connector, DXGDRM_MAX_DIMENSION,
				     DXGDRM_MAX_DIMENSION);

	/* The preferred mode need not be one of the standard ones. */
	mode = drm_cvt_mode(connector->dev, width, height, 60, false, false,
			    false);
	if (mode) {
		mode->type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
		drm_mode_probed_add(connector, mode);
		count++;
	}

	return count;
}

static const struct drm_connector_helper_funcs dxgdrm_connector_helper_funcs = {
	.get_modes	= dxgdrm_connector_get_modes,
};

static const struct drm_connector_funcs dxgdrm_connector_funcs = {
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs dxgdrm_mode_config_funcs = {
	.fb_create	= drm_gem_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= drm_atomic_helper_commit,
};

static int dxgdrm_modeset_init(struct dxgdrm_device *dxg)
{
	struct drm_device *dev = &dxg->base;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.funcs = &dxgdrm_mode_config_funcs;
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = DXGDRM_MAX_DIMENSION;
	dev->mode_config.max_height = DXGDRM_MAX_DIMENSION;
	dev->mode_config.cursor_width = DXGDRM_CURSOR_SIZE;
	dev->mode_config.cursor_height = DXGDRM_CURSOR_SIZE;
	dev->mode_config.preferred_depth = 24;

	ret = drm_universal_plane_init(dev, &dxg->primary, 1,
				       &dxgdrm_plane_funcs,
				       dxgdrm_primary_formats,
				       ARRAY_SIZE(dxgdrm_primary_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&dxg->primary, &dxgdrm_primary_helper_funcs);
	drm_plane_enable_fb_damage_clips(&dxg->primary);

	ret = drm_universal_plane_init(dev, &dxg->cursor, 1,
				       &dxgdrm_plane_funcs,
				       dxgdrm_cursor_formats,
				       ARRAY_SIZE(dxgdrm_cursor_formats),
				       NULL, DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&dxg->cursor, &dxgdrm_cursor_helper_funcs);
	ret = dxgdrm_cursor_create_hotspot_properties(&dxg->cursor);
	if (ret)
		return ret;

	ret = drm_crtc_init_with_planes(dev, &dxg->crtc, &dxg->primary,
					&dxg->cursor, &dxgdrm_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&dxg->crtc, &dxgdrm_crtc_helper_funcs);

	ret = drm_simple_encoder_init(dev, &dxg->encoder,
				      DRM_MODE_ENCODER_VIRTUAL);
	if (ret)
		return ret;
	dxg->encoder.possible_crtcs = drm_crtc_mask(&dxg->crtc);

	ret = drm_connector_init(dev, &dxg->connector, &dxgdrm_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;
	drm_connector_helper_add(&dxg->connector,
				 &dxgdrm_connector_helper_funcs);

	ret = drm_connector_attach_encoder(&dxg->connector, &dxg->encoder);
	if (ret)
		return ret;

	drm_mode_config_reset(dev);
	return 0;
}

/*
 * The presenter's side.
 *
 * Spike shortcut: both ioctls are allowed on the render node, so anything that
 * can render can also read the screen. The real thing wants a node or a
 * capability of its own.
 */

static int dxgdrm_get_frame_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file)
{
	struct dxgdrm_device *dxg = to_dxgdrm(dev);
	struct drm_dxgdrm_get_frame *args = data;
	struct drm_framebuffer *fb;
	u64 seen = args->seq;
	long timeout = MAX_SCHEDULE_TIMEOUT;
	long ret;
	int fd = -1, cursor_fd = -1;

	if (args->timeout_ms)
		timeout = msecs_to_jiffies(args->timeout_ms);

	ret = wait_event_interruptible_timeout(dxg->frame_wq,
					       READ_ONCE(dxg->seq) != seen,
					       timeout);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIME;

	memset(args, 0, sizeof(*args));
	args->fd = -1;
	args->cursor_fd = -1;

	mutex_lock(&dxg->frame_lock);

	fb = dxg->primary_fb;
	if (fb) {
		struct dxgdrm_gem *gem = to_dxgdrm_gem(fb->obj[0]);

		if (gem->shared) {
			fd = get_unused_fd_flags(O_CLOEXEC);
			if (fd < 0) {
				mutex_unlock(&dxg->frame_lock);
				return fd;
			}
			fd_install(fd, get_file(gem->shared));
			args->fd = fd;
			args->flags |= DXGDRM_FRAME_SHARED;
		} else {
			args->flags |= DXGDRM_FRAME_DUMB;
		}

		args->flags |= DXGDRM_FRAME_PRIMARY;
		args->buffer_id = gem->id;
		args->width = fb->width;
		args->height = fb->height;
		args->format = fb->format->format;
		args->pitch = fb->pitches[0];
	}

	if (dxg->damage_full)
		args->flags |= DXGDRM_FRAME_DAMAGE_FULL;
	args->num_damage = dxg->num_damage;
	memcpy(args->damage, dxg->damage,
	       dxg->num_damage * sizeof(dxg->damage[0]));
	dxg->num_damage = 0;
	dxg->damage_full = false;

	fb = dxg->cursor_fb;
	if (fb) {
		struct dxgdrm_gem *gem = to_dxgdrm_gem(fb->obj[0]);

		/* KWin renders the cursor on the GPU like any other layer. */
		if (gem->shared) {
			cursor_fd = get_unused_fd_flags(O_CLOEXEC);
			if (cursor_fd < 0) {
				mutex_unlock(&dxg->frame_lock);
				if (fd >= 0)
					close_fd(fd);
				return cursor_fd;
			}
			fd_install(cursor_fd, get_file(gem->shared));
			args->cursor_fd = cursor_fd;
			args->flags |= DXGDRM_FRAME_CURSOR_SHARED;
		}

		args->flags |= DXGDRM_FRAME_CURSOR;
		args->cursor_buffer_id = gem->id;
		args->cursor_x = dxg->cursor_x;
		args->cursor_y = dxg->cursor_y;
		args->cursor_hot_x = dxg->cursor_hot_x;
		args->cursor_hot_y = dxg->cursor_hot_y;
		args->cursor_width = fb->width;
		args->cursor_height = fb->height;
		args->cursor_format = fb->format->format;
		args->cursor_pitch = fb->pitches[0];
	}

	args->seq = dxg->seq;
	args->primary_seq = dxg->primary_seq;
	args->cursor_seq = dxg->cursor_seq;

	mutex_unlock(&dxg->frame_lock);
	return 0;
}

static int dxgdrm_read_pixels_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct dxgdrm_device *dxg = to_dxgdrm(dev);
	struct drm_dxgdrm_read_pixels *args = data;
	struct drm_framebuffer *fb;
	struct dxgdrm_gem *gem;
	u64 size;
	int ret = 0;

	mutex_lock(&dxg->frame_lock);
	switch (args->plane) {
	case DXGDRM_PLANE_PRIMARY:
		fb = dxg->primary_fb;
		break;
	case DXGDRM_PLANE_CURSOR:
		fb = dxg->cursor_fb;
		break;
	default:
		fb = NULL;
		ret = -EINVAL;
		break;
	}
	if (fb)
		drm_framebuffer_get(fb);
	mutex_unlock(&dxg->frame_lock);

	if (!fb)
		return ret ?: -ENOENT;

	gem = to_dxgdrm_gem(fb->obj[0]);
	size = (u64)fb->pitches[0] * fb->height;
	if (!gem->vaddr || fb->offsets[0] || size > gem->base.size) {
		ret = -EINVAL;
	} else if (size > args->size) {
		ret = -ENOSPC;
	} else if (copy_to_user(u64_to_user_ptr(args->data), gem->vaddr, size)) {
		ret = -EFAULT;
	}

	args->width = fb->width;
	args->height = fb->height;
	args->format = fb->format->format;
	args->pitch = fb->pitches[0];

	drm_framebuffer_put(fb);
	return ret;
}

static const struct drm_ioctl_desc dxgdrm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(DXGDRM_FENCE_FROM_EVENTFD, dxgdrm_fence_from_eventfd,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(DXGDRM_GET_FRAME, dxgdrm_get_frame_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(DXGDRM_READ_PIXELS, dxgdrm_read_pixels_ioctl,
			  DRM_RENDER_ALLOW),
};

static const struct file_operations dxgdrm_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
	.compat_ioctl	= drm_compat_ioctl,
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= noop_llseek,
	/* Dumb buffers only, see dxgdrm_gem_mmap(). */
	.mmap		= drm_gem_mmap,
	/* drm_open_helper() rejects the open with -EINVAL if this is unset
	 * (drm_file.c:329). DRM_GEM_FOPS sets it; a hand-rolled fops has to say
	 * so itself. */
	.fop_flags	= FOP_UNSIGNED_OFFSET,
};

/*
 * DRIVER_SYNCOBJ_TIMELINE is what mutter checks before advertising
 * wp_linux_drm_syncobj_v1 (meta-wayland-linux-drm-syncobj.c:521): it wants
 * drmGetCap(DRM_CAP_SYNCOBJ_TIMELINE) to be true and drmSyncobjEventfd() to be
 * present. Both are answered entirely by drm core -- every DRM_IOCTL_SYNCOBJ_*
 * lives in drm_syncobj.c behind nothing but these feature bits (drm_ioctl.c:698)
 * -- so timeline syncobjs, sync_file import/export and eventfd signalling all
 * come from setting two flags. The driver supplies no callbacks for any of it.
 */
static const struct drm_driver dxgdrm_driver = {
	.driver_features	= DRIVER_RENDER | DRIVER_GEM |
				  DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE |
				  DRIVER_MODESET | DRIVER_ATOMIC,
	.dumb_create		= dxgdrm_dumb_create,
	.prime_fd_to_handle	= dxgdrm_prime_fd_to_handle,
	.fops			= &dxgdrm_fops,
	.ioctls			= dxgdrm_ioctls,
	.num_ioctls		= ARRAY_SIZE(dxgdrm_ioctls),
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.major			= 1,
	.minor			= 0,
	.patchlevel		= 0,
};

static int dxgdrm_probe(struct platform_device *pdev)
{
	struct dxgdrm_device *dxg;
	int ret;

	dxg = devm_drm_dev_alloc(&pdev->dev, &dxgdrm_driver,
				 struct dxgdrm_device, base);
	if (IS_ERR(dxg))
		return PTR_ERR(dxg);

	platform_set_drvdata(pdev, dxg);

	mutex_init(&dxg->frame_lock);
	init_waitqueue_head(&dxg->frame_wq);

	ret = dxgdrm_modeset_init(dxg);
	if (ret)
		return ret;

	ret = drm_dev_register(&dxg->base, 0);
	if (ret)
		return ret;

	drm_info(&dxg->base, "render node and %ux%u virtual display registered for /dev/dxg\n",
		 width, height);
	return 0;
}

static void dxgdrm_remove(struct platform_device *pdev)
{
	struct dxgdrm_device *dxg = platform_get_drvdata(pdev);

	drm_dev_unregister(&dxg->base);
	/* Disables the planes, which drops the framebuffers we hold. */
	drm_atomic_helper_shutdown(&dxg->base);
}

static struct platform_driver dxgdrm_platform_driver = {
	.probe	= dxgdrm_probe,
	.remove	= dxgdrm_remove,
	.driver	= {
		.name = "dxgdrm",
	},
};

static struct platform_device *dxgdrm_pdev;

static int __init dxgdrm_init(void)
{
	int ret;

	dxgdrm_wq = alloc_workqueue("dxgdrm", 0, 0);
	if (!dxgdrm_wq)
		return -ENOMEM;

	ret = platform_driver_register(&dxgdrm_platform_driver);
	if (ret)
		goto err_wq;

	dxgdrm_pdev = platform_device_register_simple("dxgdrm", -1, NULL, 0);
	if (IS_ERR(dxgdrm_pdev)) {
		ret = PTR_ERR(dxgdrm_pdev);
		goto err_driver;
	}

	return 0;

err_driver:
	platform_driver_unregister(&dxgdrm_platform_driver);
err_wq:
	destroy_workqueue(dxgdrm_wq);
	return ret;
}

static void __exit dxgdrm_exit(void)
{
	platform_device_unregister(dxgdrm_pdev);
	platform_driver_unregister(&dxgdrm_platform_driver);
	/* Every fence holds a module reference until its release work runs,
	 * so by now only the tail of those work items can be left. */
	destroy_workqueue(dxgdrm_wq);
	/* kfree_rcu() needs nothing from us, but be tidy. */
	rcu_barrier();
}

module_init(dxgdrm_init);
module_exit(dxgdrm_exit);

MODULE_IMPORT_NS("DMA_BUF");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
