# 0007. D3D12 fences become sync_files through an eventfd watcher

Status: Accepted

## Context

D3D12 under WSL reports completion through an eventfd
(`ID3D12Fence::SetEventOnCompletion`). There is no `dma_fence` anywhere in the
WSL GPU stack. Everything on the Linux side wants a sync_file: `drm_syncobj`
only imports one, and Chromium's explicit-sync path drops the frame when the
import fails (`wayland_surface.cc:444`). An eventfd polls the same way, but
fails `SYNC_IOC_FILE_INFO`, cannot be imported and cannot be merged.

## Decision

`DXGDRM_FENCE_FROM_EVENTFD` takes an eventfd and returns a sync_file whose
fence signals when the eventfd does.

- The mechanism is the one KVM's irqfd uses: a wait queue entry on the eventfd
  through `vfs_poll()`. The eventfd is never read, because a read would
  consume the count Mesa's own waiter looks for.
- Only the `eventfd_ctx` is held, not the file, so closing the last reference
  wakes the fence with `EPOLLHUP`, and it signals.
- Each fence has its own fence context. `sync_file_merge()` and `dma_resv`
  keep only the newest fence per context, so a shared context would let a
  merged fence signal early (`44d9d4c`).
- The fence is signalled from an `irq_work`, not inside the eventfd wakeup. A
  syncobj eventfd chained off it would otherwise hit `eventfd_signal()`'s
  recursion check and be dropped.
- Fences are freed with `kfree_rcu()`, on a workqueue that module exit drains,
  and each holds a module reference.
- A watchdog signals the fence with `-ETIMEDOUT` after `fence_timeout_ms`
  (10 s by default).

## Consequences

- Chromium's explicit sync, `drm_syncobj` and weaselwayd's readback fence
  (weaselway 0007) work on d3d12.
- A GPU job that takes longer than `fence_timeout_ms` signals as failed.
