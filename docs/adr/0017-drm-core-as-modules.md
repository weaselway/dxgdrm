# 0017. DRM core is built as modules for kernels without it

Status: Accepted

## Context

Up to `6.18.35.2` the WSL kernel had `CONFIG_DRM=y`. From `6.18.40.1` (WSL
3.0) on it is unset, and WSL's module image does not provide it either, so
dxgdrm has nothing to link against. The modules are loaded into Microsoft's
kernel, so `vmlinux` must stay the one WSL ships. Enabling `DRM` normally
changes it, because `DRM` selects `HDMI`, a bool that puts `hdmi.o` into
`vmlinux`.

## Decision

- For a kernel whose config has `CONFIG_DRM` unset, `build-drm-modules.sh`
  builds `hdmi.ko`, `drm.ko` and `drm_kms_helper.ko` from the same tree
  (`315e0e4`). The flake and the container build both use it.
- `kernel-drm-as-modules.patch` makes `HDMI` a tristate, so it follows `DRM=m`
  into a module, and gives `DRM_KMS_HELPER` a prompt.
- `vmlinux` is built first from the stock config and not rebuilt. The three
  modules are built as single targets. `DRM_FBDEV_EMULATION` is off, because
  it would select a built-in console option.
- The script fails if enabling `DRM=m` changes any option outside DRM other
  than `HDMI=m`.
- The package installs them next to `dxgdrm.ko` and runs `depmod`, so
  `modprobe -d <package> dxgdrm` loads all four in order (see 0005).

## Consequences

- Checked once for `6.18.40.1` by building `vmlinux` both ways: all 11526
  exported symbols kept their CRC.
- `drm.ko` has to be loaded before systemd-logind starts, or logind refuses the
  compositor the KMS node (weaselway 0022).
- Whether a kernel needs this is read from its config. Nothing has to be
  configured per kernel.
- The container build and the CI workflow were untested for this when it was
  added.
