# 0003. Build with the WSL kernel's own toolchain

Status: Accepted

## Context

WSL ships no `/lib/modules/$(uname -r)/build` and no headers package. The WSL
kernel sets `CONFIG_MODVERSIONS`, and the symbol CRCs depend on the compiler.
A module built with Fedora's gcc, and later with nixpkgs' gcc, was rejected
("disagrees about version of symbol module_layout"). `--force-modversion`
loads it, but taints the kernel.

`/proc/version` and `CONFIG_CC_VERSION_TEXT` report `gcc (GCC) 13.2.0`. The
bare `(GCC)` means a vanilla upstream build. No distribution's compiler looks
like that.

## Decision

- The module and the kernel tree it is built against are compiled with the
  kernel.org crosstool gcc 13.2.0 (`3ebd9f7`). The flake also uses its
  binutils 2.41 (`f9642ec`); `build-kernel-headers.sh` takes only `CC` from
  it. The download is pinned by SHA256.
- `build-kernel-headers.sh` (and `packages.wsl-kernel-dev` in the flake)
  fetches the `microsoft/WSL2-Linux-Kernel` tag of the release, builds
  `vmlinux` and `modules_prepare` with that compiler, and checks
  `kernel.release`.
- One place decides the compiler for both sides: the Makefile defaults `KGCC`
  to the toolchain the script fetched, and the flake's dev shell sets it.
- The flake's configure phase runs `olddefconfig` and fails if the `.config`
  differs from the captured one, apart from a few lines that cannot change a
  CRC. A toolchain mismatch shows up before the long `vmlinux` build. The
  check runs only on x86_64 hosts, where kgcc is used.
- The flake runs kgcc's binaries through `ld.so` wrappers rather than
  patchelf, which qemu-user cannot map on a 16K-page host.

## Consequences

- The module loads without forcing, and can be built once and shipped to
  every machine on the same kernel release.
- kernel.org ships x86_64 host binaries only. On other hosts the flake falls
  back to a cross gcc, which compiles the module, but it will not load.
- Preparing the kernel tree takes about 6 minutes and 1.8 GB.
