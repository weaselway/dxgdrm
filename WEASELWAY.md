# Building dxgdrm with Nix

A compile check for the module against the WSL2 kernel, using
[flake.nix](flake.nix) (nixpkgs `nixos-26.05`). Nothing is loaded or
installed. **Modules built this way are not for loading.** Use
[docker-env.sh](docker-env.sh) for anything that goes onto a real WSL kernel
(see "Why not for loading" below).

```sh
nix develop -c make          # builds ./dxgdrm.ko against the prepared kernel tree
nix develop -c make clean
nix build .#dxgdrm           # same, as a derivation: result/lib/modules/<release>/extra/dxgdrm.ko
```

## What the flake does

- **`packages.wsl-kernel-dev`** is the Nix equivalent of
  [build-kernel-headers.sh](build-kernel-headers.sh). It fetches
  `microsoft/WSL2-Linux-Kernel` at tag `linux-msft-wsl-6.18.33.2`, uses
  `conf/kernel-6.18.33.2-microsoft-standard-WSL2.conf` as `.config`, runs
  `make LOCALVERSION= vmlinux modules_prepare`, copies
  `vmlinux.symvers` to `Module.symvers`, and checks that `kernel.release`
  matches.
  - It keeps `vmlinux` without its DWARF debug info, but with `.BTF`, which
    `CONFIG_DEBUG_INFO_BTF_MODULES` needs to generate module BTF.
  - About 6 minutes to build, 1.8 GB in the store. It is built once and then
    reused.
- **The dev shell** exports `KDIR` (that store path), `ARCH=x86_64`,
  `CROSS_COMPILE=x86_64-unknown-linux-gnu-` and
  `KGCC=x86_64-unknown-linux-gnu-gcc`. The [Makefile](Makefile) picks all of
  them up (`KDIR ?=`, `KGCC ?=`), so plain `make` works.
- **The compiler** is always a cross gcc targeting x86_64
  (`pkgsCross.gnu64`), so aarch64 hosts build x86_64 modules too.

## Targeting another kernel release

Add the captured config to `conf/`, then in [flake.nix](flake.nix):

1. Set `kernelRelease` to match the config's file name.
2. Update the `fetchFromGitHub` hash. Get it with
   `nix flake prefetch github:microsoft/WSL2-Linux-Kernel/linux-msft-wsl-<version>`.

## Why not for loading

`CONFIG_MODVERSIONS=y`, and the symbol CRCs depend on the compiler. The WSL
kernel is built with a vanilla gcc 13.2.0, while this uses nixpkgs' gcc 15.x,
so the kernel rejects the module ("disagrees about version of symbol").
Struct layouts and API are the same, so this is a faithful compile check. See
[BUILD-NOTES.md](BUILD-NOTES.md) for the details.

## Notes

- `nix build` leaves a `result` symlink. It is also the GC root that keeps the
  prepared kernel tree from being garbage-collected. Deleting it frees the
  space, and the next build redoes the kernel.
