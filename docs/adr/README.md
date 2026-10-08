# Decision records (ADR)

Each file records one decision: the context, the decision and its
consequences, including known risks. They were written after the fact, from
the header comments in `dxgdrm.c` and `dxgdrm_drm.h`, BUILD-NOTES.md and the
commit messages.

New decisions get the next number. A changed decision gets a new record, and
the old one gets the status "Superseded by NNNN". Records of the distribution
and of weaselwayd are in weaselway's `docs/adr`, and are referred to as
"weaselway NNNN".

| No. | Decision | Status |
|---|---|---|
| [0001](0001-render-node-for-d3d12.md) | A DRM render node for the d3d12 driver, which allocates no GPU memory | Accepted |
| [0002](0002-driver-name.md) | The driver is named `dxgdrm` | Accepted |
| [0003](0003-build-with-the-kernels-toolchain.md) | Build with the WSL kernel's own toolchain | Accepted |
| [0004](0004-container-build-no-install-step.md) | The container build runs as the invoking user and stops before loading | Accepted |
| [0005](0005-not-lib-modules.md) | The module is loaded by path, not from `/lib/modules` | Accepted |
| [0006](0006-one-build-per-captured-config.md) | One build per captured kernel config, keyed on the release | Accepted |
| [0007](0007-fences-from-eventfds.md) | D3D12 fences become sync_files through an eventfd watcher | Accepted |
| [0008](0008-syncobjs-from-drm-core.md) | Syncobjs come from DRM core, through feature bits only | Accepted |
| [0009](0009-uapi-extends-by-zero-extension.md) | The uapi structs grow at the end, and new fields must be zero | Accepted |
| [0010](0010-kernel-tree-out-of-the-closure.md) | The kernel tree stays out of the module's runtime closure | Accepted |
| [0011](0011-virtual-kms-display.md) | A virtual KMS display whose commits go to a userspace presenter | Accepted |
| [0012](0012-shared-handles-and-dumb-buffers.md) | Framebuffers are wrapped D3D12 shared handles or dumb buffers | Accepted |
| [0013](0013-flip-waits-for-the-presenter.md) | A page flip completes when the presenter has taken the frame | Accepted |
| [0014](0014-presenter-ioctls-on-the-render-node.md) | The presenter's ioctls are allowed on the render node | Accepted (spike shortcut) |
| [0015](0015-cursor-plane-behaviour.md) | Hotspot properties without `DRIVER_CURSOR_HOTSPOT`, and only new frames count | Accepted |
| [0016](0016-udev-mode-only.md) | The udev rule sets mode 0666 on both nodes, and no group | Accepted |
| [0017](0017-drm-core-as-modules.md) | DRM core is built as modules for kernels without it | Accepted |

## Known risks

Found while writing these records, details in the linked files:

- Any local process can read the screen, set the mode and acknowledge frames:
  the presenter's ioctls are allowed on the render node, and the udev rule
  makes it mode 0666 ([0014](0014-presenter-ioctls-on-the-render-node.md),
  [0016](0016-udev-mode-only.md)).
- Each WSL kernel release needs its own build, and an update breaks loading
  until one is added ([0006](0006-one-build-per-captured-config.md)).
- Two presenters at once take frames and acknowledgements from each other
  ([0014](0014-presenter-ioctls-on-the-render-node.md)).
- On a host other than x86_64 the module compiles but does not load
  ([0003](0003-build-with-the-kernels-toolchain.md)).
- A GPU job that takes longer than `fence_timeout_ms` signals its fence as
  failed ([0007](0007-fences-from-eventfds.md)).
- A client that draws in place without `FB_DAMAGE_CLIPS` is not seen
  ([0015](0015-cursor-plane-behaviour.md)).
- The container build and CI were untested for kernels without DRM core when
  that support was added ([0017](0017-drm-core-as-modules.md)).
