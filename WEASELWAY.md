# Building dxgdrm with Nix

Builds the module against the WSL2 kernel with [flake.nix](flake.nix)
(nixpkgs `nixos-26.05`), using the same kernel.org gcc 13.2.0 and binutils
2.41 the WSL kernel was built with, so the MODVERSIONS CRCs should match and
the module should load (see "Loading" below). The NixOS-WSL image in
[weaselway](https://github.com/weaselway/weaselway) takes it from here.

```sh
nix develop -c make          # builds ./dxgdrm.ko against the prepared kernel tree
nix develop -c make clean
nix build .#dxgdrm           # same, as a derivation: result/lib/modules/<release>/extra/dxgdrm.ko
nix build                    # dxgdrm-all: one module per kernel in conf/
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
- **The compiler** is `packages.kgcc`: the kernel.org crosstool gcc
  13.2.0, whose `--version` matches the kernel's `CONFIG_CC_VERSION_TEXT`
  (`gcc (GCC) 13.2.0`), plus the binutils 2.41 it ships. The kernel config's
  `CONFIG_LD_VERSION` is 2.41 too. Its binaries run through small wrapper
  scripts that call nixpkgs' `ld.so`, not through patchelf, because a
  patchelf'd non-PIE binary can't be mapped by qemu-user on a 16K-page host.
  kernel.org only ships x86_64 host binaries, so on other hosts the flake
  falls back to `pkgsCross.gnu64`: that still compiles the module but it won't
  load, see below.
- **The config check.** The configure phase runs `olddefconfig` and fails if
  the resulting `.config` differs from the captured one. Only a few lines are
  allowed to differ, none of which can change a CRC: the header comment,
  `PAHOLE_VERSION`, `CC_CAN_LINK` (kgcc has no libc) and
  `DEBUG_INFO_COMPRESSED_ZSTD`. Any other difference means a different
  toolchain, and it is caught before the long vmlinux build.
- **The dev shell** exports `KDIR` (the newest kernel's prepared tree),
  `ARCH=x86_64`, `CROSS_COMPILE` and `KGCC` (both pointing at kgcc). The
  [Makefile](Makefile) picks them up (`KDIR ?=`, `KGCC ?=`), so plain `make`
  works.

## Targeting another kernel release

Add the captured config to `conf/` as `kernel-<release>.conf`, then add
`"<release>" = "<hash>";` to `kernels` in [flake.nix](flake.nix). Get the hash
with `nix flake prefetch github:microsoft/WSL2-Linux-Kernel/linux-msft-wsl-<version>`.
`dxgdrm-all` then carries a module for that release as well, and the loader
picks the one matching `uname -r`.

## Loading

`CONFIG_MODVERSIONS=y`, and the symbol CRCs depend on the compiler, so a module
built with nixpkgs' gcc 15 is rejected ("disagrees about version of symbol").
kgcc avoids that, and the config check proves that the toolchain matches. The
built module's vermagic is `6.18.33.2-microsoft-standard-WSL2 SMP preempt
mod_unload modversions`. Loading it on a real WSL kernel hasn't been tried yet.
See [BUILD-NOTES.md](BUILD-NOTES.md) for background.

## Notes

- `nix build` leaves a `result` symlink. It is also the GC root that keeps the
  prepared kernel tree from being garbage-collected. Deleting it frees the
  space, and the next build redoes the kernel.
