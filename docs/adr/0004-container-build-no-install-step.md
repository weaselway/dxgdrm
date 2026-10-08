# 0004. The container build runs as the invoking user and stops before loading

Status: Accepted

## Context

Building needs the kernel source, a matching toolchain (see 0003) and the
kernel's build dependencies, none of which should be required on the host.
Loading a module, however, has to happen on the host: a container shares the
host's kernel.

## Decision

- `docker-env.sh` builds the kernel tree and `dxgdrm.ko` in a container. It
  bind-mounts the repository, so `./build` persists, and nothing else.
- The container runs as `$(id -u):$(id -g)`, with the image built for those
  ids, so the results are owned by the user rather than root.
- The script stops after building. Loading is a `modprobe` typed on the host.
- The Makefile builds and cleans, nothing else. The install target was
  removed because nothing written to `/lib/modules` survives a WSL restart
  (`724a6c2`, see 0005). The load and unload targets followed (`d989bef`):
  whether `--force-modversion` is needed depends on what compiled the `.ko`
  on disk, which the build invocation does not know.
- The container's distribution is Ubuntu 26.04. It is not load-bearing, since
  the compiler comes from kernel.org.

## Consequences

- The only host requirements are Docker and `sudo` for `modprobe`.
- Deploying the module is weaselway's job (see 0006 and weaselway 0022). This repository builds
  it and gets it loaded for a look.
