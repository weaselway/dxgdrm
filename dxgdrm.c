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
#include <linux/file.h>
#include <linux/irq_work.h>
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

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
