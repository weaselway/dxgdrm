# 0022. Kernel trees come from wsl-kernel, with a pinned nixpkgs

Status: Accepted

## Context

The flake built the prepared kernel trees itself, with weaselway's nixpkgs
through `follows`. Each nixpkgs update that changed stdenv rebuilt both trees
in weaselway's CI, although the modules depend only on the kernel source, its
config and kgcc. weaselway's Bluetooth modules (weaselway 0035) need the same
trees and copied the kbuild setup from here.

## Decision

- The trees, kgcc, the configs, `kernels` and `add-kernel.sh` live in
  [wsl-kernel](https://github.com/weaselway/wsl-kernel). Its nixpkgs is pinned
  and nobody overrides it with `follows`; this flake's `nixpkgs` follows it.
- The trees are stock: Microsoft's source and captured config, nothing else.
- wsl-kernel's `mkModules` builds modules in a copy of a tree, without
  rebuilding `vmlinux`. dxgdrm uses it for one derivation per release, which
  applies `kernel-drm-as-modules.patch`, runs `build-drm-modules.sh` (0017)
  and then builds dxgdrm with `M=`.
- The dev shell's `KDIR` is a tree with the DRM modules built, made on demand.
- CI's container build checks out wsl-kernel at the revision `flake.lock`
  pins, for its configs.

## Consequences

- A nixpkgs update elsewhere no longer rebuilds the trees. Using the pinned
  nixpkgs means evaluating weaselway downloads a second nixpkgs source.
- Adding a kernel touches three repos: wsl-kernel, then dxgdrm's lock, then
  weaselway's.
- dxgdrm and the Bluetooth modules no longer share a tree with the DRM patch
  applied. Each derivation copies a 1.8 GB tree to build in.
- The modules are no longer byte-identical to earlier builds, because the
  build directory changed: the DRM modules differ in their build ID only,
  dxgdrm.ko also in the header paths of its `WARN()` strings. Symbol CRCs
  and modinfo are the same (checked for both releases).
