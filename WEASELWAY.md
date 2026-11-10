# Building dxgdrm with Nix

Builds the module against the WSL2 kernel with [flake.nix](flake.nix). The
prepared kernel trees, the compiler and the module builder come from
[wsl-kernel](https://github.com/weaselway/wsl-kernel), which uses the same
kernel.org gcc 13.2.0 and binutils 2.41 the WSL kernel was built with, so the
MODVERSIONS CRCs match and the module loads (see "Loading" below). The
NixOS-WSL image in [weaselway](https://github.com/weaselway/weaselway) takes it
from here.

```sh
nix develop -c make          # builds ./dxgdrm.ko against the newest kernel
nix develop -c make clean
nix build .#dxgdrm           # same, as a derivation: result/lib/modules/<release>/extra/dxgdrm.ko
nix build                    # dxgdrm-all: one module per kernel wsl-kernel supports
```

## What the flake does

- **The kernel trees** are wsl-kernel's `wsl-kernel-dev-<release>`: the
  `microsoft/WSL2-Linux-Kernel` tag built with its captured config as far as
  a module build needs, with a check that the config and `kernel.release`
  match the shipped kernel. wsl-kernel pins its own nixpkgs, so nixpkgs
  updates elsewhere do not rebuild them. Its README has the details.
- **The module** is built with wsl-kernel's `mkModules`, in a copy of the
  tree, one derivation per release.
- **DRM core.** A kernel whose config has `CONFIG_DRM` unset (`6.18.40.1`)
  gets `hdmi.ko`, `drm.ko` and `drm_kms_helper.ko` built in the same
  derivation before dxgdrm, after applying
  [kernel-drm-as-modules.patch](kernel-drm-as-modules.patch) and without
  rebuilding `vmlinux`. One with `CONFIG_DRM=y` but no `CONFIG_DRM_KMS_HELPER`
  (`6.18.54.1`) gets only `drm_kms_helper.ko`, see
  [BUILD-NOTES.md](BUILD-NOTES.md#kernels-without-drm-core). The package
  carries them next to `dxgdrm.ko` with a `modules.dep`, so it is a module
  root of its own: `modprobe -d result dxgdrm` loads all four in order.
- **The dev shell** exports `KDIR`, `ARCH=x86_64`, `CROSS_COMPILE` and `KGCC`.
  `KDIR` is the newest tree with the DRM modules built, which `nix develop`
  builds on first use. The [Makefile](Makefile) picks them up (`KDIR ?=`,
  `KGCC ?=`), so plain `make` works.

## Targeting another kernel release

Add it to wsl-kernel with its `add-kernel.sh`, then `nix flake update
wsl-kernel` here. `dxgdrm-all` then carries a module for that release as
well, and the loader picks the one matching `uname -r`. Whether the release
needs DRM core or the KMS helpers built as modules is read from its config.

## Loading

`CONFIG_MODVERSIONS=y`, and the symbol CRCs depend on the compiler, so a module
built with nixpkgs' gcc 15 is rejected ("disagrees about version of symbol").
kgcc avoids that, and the config check proves that the toolchain matches. The
built module's vermagic is `6.18.54.1-microsoft-standard-WSL2 SMP preempt
mod_unload modversions`, and it loads on that WSL kernel. See [BUILD-NOTES.md](BUILD-NOTES.md) for background.

## Notes

- The packages do not reference the kernel trees, which are 1.8 GB each, so
  garbage collection removes them. The next build fetches them from a binary
  cache or rebuilds them.
