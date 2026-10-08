# 0006. One build per captured kernel config, keyed on the release

Status: Accepted

## Context

Everyone on a given WSL release runs the same Microsoft-built kernel, so a
module built for that release loads on every such machine. A WSL update moves
the release string, and the old module is rejected. CI runners are not WSL, so
`uname -r` cannot tell the build which kernel to target.

## Decision

- `conf/kernel-<release>.conf` holds the config of each supported kernel,
  named after its release. The release comes from the file name, because the
  config does not state it, and is checked against the prepared tree's
  `kernel.release`.
- `KERNEL_CONFIG` makes `build-kernel-headers.sh` use such a file instead of
  the running kernel.
- CI builds one matrix leg per file in `conf/`. It caches the prepared kernel
  tree keyed on the release and the config's hash.
- The flake has a `kernels` attribute with the source hash of each release.
  `dxgdrm-all` contains one module per release, at
  `lib/modules/<release>/extra/dxgdrm.ko`, plus the udev rules. For kernels
  without DRM core it also carries those modules and a `modules.dep` (see
  0017).

## Consequences

- Adding a kernel means adding its config to `conf/` and its hash to
  `kernels`. Nothing else names a kernel version.
- The loader picks the module matching `uname -r`, and a kernel without a
  build is a missing file, not a vermagic error.
- Supported today: `6.18.33.2-microsoft-standard-WSL2` and
  `6.18.40.1-microsoft-standard-WSL2`, x86_64 only. A custom kernel set with
  `kernel=` in `.wslconfig` is not covered.
