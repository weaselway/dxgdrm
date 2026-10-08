# 0005. The module is loaded by path, not from /lib/modules

Status: Accepted

## Context

WSL mounts `/usr/lib/modules/$(uname -r)` itself at boot, as an overlay whose
lower layer is WSL's module image and whose upper layer lives in WSL's init
mount namespace. A module copied into `.../extra`, and the `depmod` index that
points at it, is gone at the next `wsl --shutdown`. It looks like it worked
until the reboot.

## Decision

- There is no install step. The module is loaded from a directory on the
  distro's own disk: `modprobe ./dxgdrm.ko` for a local build (the slash makes
  modprobe treat the argument as a file).
- The flake's package is a module root of its own, laid out as
  `lib/modules/<release>/`, with its own `modules.dep`.
  `modprobe -d <package> dxgdrm` uses it in place of `/`.

## Consequences

- The module survives a restart of WSL, and something has to load it once per
  boot. In weaselway that is `weaselway-prep.service`.
- Loading by file path skips `modules.dep`. That is fine while dxgdrm depends
  only on built-in code. Kernels without DRM core need the module root (see
  0017).
