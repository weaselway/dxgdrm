# 0010. The kernel tree stays out of the module's runtime closure

Status: Accepted

## Context

The module's debug info, and a `__FILE__` string from an inline kernel header,
contain store paths of the prepared kernel tree. Nix therefore counted the
tree, and kgcc and gcc through it, as runtime dependencies of the module:
2 GB of weaselway's NixOS-WSL image, which GitHub limits to 2 GiB per release
asset.

## Decision

- The flake strips the module's debug info. `.BTF` and the symbol versions
  stay.
- The remaining store hash is blanked.
- The package forbids references to the kernel tree and the toolchain, so the
  build fails if one comes back.

## Consequences

- The module adds almost nothing to the image.
- Debugging a crash in the module needs a local build with debug info.
- The `result` link of a local `nix build` is the GC root that keeps the
  kernel tree. Deleting it frees the space, and the next build redoes the
  tree.
