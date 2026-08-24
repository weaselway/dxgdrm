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
 * That is the entire requirement. Chromium wants a path it can open, identify
 * and hand to gbm_create_device(); it never issues a driver-specific ioctl,
 * because every buffer still comes from d3d12 through CreateSharedHandle. So
 * this driver allocates nothing and implements no ioctls of its own. It exists
 * to be identified.
 *
 * Deliberately absent:
 *
 *   - Dumb buffers. If kms_swrast could allocate through this node, Mesa's
 *     dri_screen_create_sw() would stop there and the session would get
 *     software rendering. Dumb-buffer ioctls require DRM_MASTER, which a
 *     render-only node has no way to grant, so this falls out of DRIVER_RENDER
 *     without a modeset feature bit -- but it is the reason not to add one.
 *
 *   - GEM object creation. DRIVER_GEM is set so that core GEM/PRIME ioctls are
 *     present and the handle table exists, but nothing here ever produces an
 *     object to put in it.
 *
 * On the name. It must not be "vgem" -- Chromium skips that node by name
 * (drm_render_node_path_finder.cc:75) -- and beyond that Chromium does not care;
 * it prefers i915/amdgpu/virtio_gpu and otherwise takes the first survivor with
 * a warning. Mesa cares more. The name goes to loader_get_driver_for_fd(), and
 * gbm_dri.c hands it to driCreateNewScreen3() as a DRI3 screen; a name the pipe
 * loader recognises would build a screen on this fd, which allocates nothing.
 * "d3d12" is tempting and wrong for a subtler reason too: libdril_dri.so is
 * installed as d3d12_dri.so and exports __driDriverGetExtensions_d3d12, so an
 * X11 DRI loader would find a stub screen through it. "dxgdrm" matches no
 * gallium driver and no installed *_dri.so, so every consumer fails to match and
 * falls through to where it was already going.
 *
 * Providing a version has one new consequence worth watching. Previously
 * dri_screen_create() bailed at loader_get_driver_for_fd() returning NULL, so
 * the zink fallback below it was unreachable. It is reachable now: on failing to
 * match the name, gbm retries as "zink" before giving up (gbm_dri.c:294). Zink
 * matches a VkPhysicalDevice to the fd via VK_EXT_physical_device_drm and should
 * find none for this device -- but if it ever did, the session would silently
 * land on lavapipe. Worth checking that gbm still reports the swrast path.
 */

#include <linux/dma-fence.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/workqueue.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>

#include "dxgdrm_drm.h"

#define DRIVER_NAME	"dxgdrm"
#define DRIVER_DESC	"Render node for the WSL d3d12 Mesa driver"

struct dxgdrm_device {
	struct drm_device base;
};


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
 * The caller keeps its eventfd. This takes its own reference and drops it when
 * the fence is released.
 */

static const char *dxgdrm_fence_get_name(struct dma_fence *fence)
{
	return "dxgdrm";
}

static void dxgdrm_fence_release_work(struct work_struct *work);

struct dxgdrm_fence {
	struct dma_fence	base;
	spinlock_t		lock;
	struct file		*efile;
	wait_queue_head_t	*wqh;
	wait_queue_entry_t	wait;
	poll_table		pt;
	struct work_struct	release_work;
};

static u64 dxgdrm_fence_context;

static void dxgdrm_fence_release(struct dma_fence *fence)
{
	struct dxgdrm_fence *f = container_of(fence, struct dxgdrm_fence, base);

	/* Both remove_wait_queue() and fput() can sleep, and a fence may be put
	 * from atomic context. */
	INIT_WORK(&f->release_work, dxgdrm_fence_release_work);
	schedule_work(&f->release_work);
}

static void dxgdrm_fence_release_work(struct work_struct *work)
{
	struct dxgdrm_fence *f =
		container_of(work, struct dxgdrm_fence, release_work);

	if (f->wqh)
		remove_wait_queue(f->wqh, &f->wait);
	if (f->efile)
		fput(f->efile);
	kfree(f);
}

static const struct dma_fence_ops dxgdrm_fence_ops = {
	.get_driver_name	= dxgdrm_fence_get_name,
	.get_timeline_name	= dxgdrm_fence_get_name,
	.release		= dxgdrm_fence_release,
};

static int dxgdrm_fence_wakeup(wait_queue_entry_t *wait, unsigned int mode,
			       int sync, void *key)
{
	struct dxgdrm_fence *f = container_of(wait, struct dxgdrm_fence, wait);
	__poll_t flags = key_to_poll(key);

	/* EPOLLHUP means the eventfd went away without ever being signalled.
	 * Signalling anyway is the only safe move: a fence that never signals
	 * hangs whoever waits on it. */
	if (flags & (EPOLLIN | EPOLLHUP))
		dma_fence_signal(&f->base);

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
	__poll_t events;
	int fd, ret;

	/* Reject anything that is not an eventfd up front: vfs_poll() would
	 * happily watch some other pollable fd and produce a fence that signals
	 * on unrelated readability. */
	ctx = eventfd_ctx_fdget(args->eventfd);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	eventfd_ctx_put(ctx);

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;

	f->efile = fget(args->eventfd);
	if (!f->efile) {
		kfree(f);
		return -EBADF;
	}

	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &dxgdrm_fence_ops, &f->lock,
		       dxgdrm_fence_context, 1);

	init_waitqueue_func_entry(&f->wait, dxgdrm_fence_wakeup);
	init_poll_funcptr(&f->pt, dxgdrm_fence_queue_proc);

	/* Queues f->wait on the eventfd's wait queue as a side effect, and
	 * reports whether it is signalled already. */
	events = vfs_poll(f->efile, &f->pt);

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

	fd_install(fd, sync_file->file);
	dma_fence_put(&f->base);

	args->fd = fd;
	return 0;

err_put_file:
	fput(sync_file->file);
	return ret;
err_put_fence:
	dma_fence_put(&f->base);
	return ret;
}

static const struct drm_ioctl_desc dxgdrm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(DXGDRM_FENCE_FROM_EVENTFD, dxgdrm_fence_from_eventfd,
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
	/* drm_open_helper() rejects the open with -EINVAL if this is unset
	 * (drm_file.c:329). DRM_GEM_FOPS sets it; a hand-rolled fops has to say
	 * so itself. Deliberately no .mmap: nothing here is mappable. */
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
				  DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE,
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

	ret = drm_dev_register(&dxg->base, 0);
	if (ret)
		return ret;

	drm_info(&dxg->base, "render node registered for /dev/dxg\n");
	return 0;
}

static void dxgdrm_remove(struct platform_device *pdev)
{
	struct dxgdrm_device *dxg = platform_get_drvdata(pdev);

	drm_dev_unregister(&dxg->base);
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

	dxgdrm_fence_context = dma_fence_context_alloc(1);

	ret = platform_driver_register(&dxgdrm_platform_driver);
	if (ret)
		return ret;

	dxgdrm_pdev = platform_device_register_simple("dxgdrm", -1, NULL, 0);
	if (IS_ERR(dxgdrm_pdev)) {
		platform_driver_unregister(&dxgdrm_platform_driver);
		return PTR_ERR(dxgdrm_pdev);
	}

	return 0;
}

static void __exit dxgdrm_exit(void)
{
	platform_device_unregister(dxgdrm_pdev);
	platform_driver_unregister(&dxgdrm_platform_driver);
}

module_init(dxgdrm_init);
module_exit(dxgdrm_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
