{
  description = "dxgdrm out-of-tree module for the WSL2 kernel (x86_64)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;

      forAllSystems =
        f: lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (system: f nixpkgs.legacyPackages.${system});

      # Every WSL kernel the module is built for. Each needs its captured config
      # in ./conf/kernel-<release>.conf and the hash of the matching
      # microsoft/WSL2-Linux-Kernel tag, see WEASELWAY.md.
      kernels = {
        "6.18.33.2-microsoft-standard-WSL2" = "sha256-5zGJZdKvTijB5guP05DRmeTh1vXgbX76y0qIUwbumX0=";
      };

      # The compiler the shipped WSL kernels are built with, the same one
      # build-kernel-headers.sh fetches: a vanilla gcc 13.2.0, see
      # BUILD-NOTES.md. MODVERSIONS CRCs depend on it, so this is what makes
      # the module loadable.
      kgccVersion = "13.2.0";

      perSystem =
        pkgs:
        let
          # kernel.org only ships the toolchain as x86_64 host binaries. On
          # other hosts fall back to nixpkgs' cross gcc, which still makes a
          # faithful compile check but not a loadable module.
          haveKgcc = pkgs.stdenv.hostPlatform.isx86_64;

          kgcc = pkgs.stdenv.mkDerivation {
            pname = "kgcc";
            version = kgccVersion;

            src = pkgs.fetchurl {
              url = "https://mirrors.edge.kernel.org/pub/tools/crosstool/files/bin/x86_64/${kgccVersion}/x86_64-gcc-${kgccVersion}-nolibc-x86_64-linux.tar.xz";
              hash = "sha256-Lxl1SWoVIA8qrvTgmYxTC7y6bKI3KFvnbwMAh9pWP7Y=";
            };

            # Drops the archive's gcc-<version>-nolibc/x86_64-linux/ prefix; gcc
            # finds its libexec relative to argv[0].
            unpackPhase = ''
              mkdir -p $out
              tar -xJf $src -C $out --strip-components=2
            '';

            # The binaries want /lib64/ld-linux-x86-64.so.2. Rather than
            # patchelf them, each executable becomes a script running the
            # original through nixpkgs' loader: a patchelf'd non-PIE binary
            # gets a load segment below 0x400000, which qemu-user cannot map
            # on a 16K-page host (Apple silicon), and that is where this gets
            # built under emulation. --argv0 keeps gcc's relative lookup of
            # cc1, as and friends pointing at the wrappers.
            installPhase =
              let
                loader = "${pkgs.glibc}/lib/ld-linux-x86-64.so.2";
                libPath = lib.makeLibraryPath [
                  pkgs.glibc
                  pkgs.zstd
                  pkgs.zlib
                  pkgs.stdenv.cc.cc.lib
                ];
              in
              ''
                runHook preInstall
                find $out -type f -executable ! -name '*.so*' -print0 | while IFS= read -r -d "" f; do
                  isELF "$f" || continue
                  dir="$(dirname "$f")"
                  real=".$(basename "$f")-wrapped"
                  mv "$f" "$dir/$real"
                  printf '#!%s\nexec %s --argv0 "$0" --library-path %s "%s/%s" "$@"\n' \
                    "${pkgs.runtimeShell}" "${loader}" "${libPath}" "$dir" "$real" > "$f"
                  chmod +x "$f"
                done
                # gcc names itself after argv[0] in --version, which the kernel
                # records as CONFIG_CC_VERSION_TEXT. The WSL kernel's says
                # plain "gcc (GCC) 13.2.0".
                ln -s x86_64-linux-gcc $out/bin/gcc
                runHook postInstall
              '';

            dontConfigure = true;
            dontBuild = true;
            dontStrip = true;
            dontPatchELF = true;

            doInstallCheck = true;
            installCheckPhase = ''
              $out/bin/x86_64-linux-gcc --version | head -n1 | grep -qx 'x86_64-linux-gcc (GCC) ${kgccVersion}'
              echo 'int f(void) { return 0; }' > t.c
              $out/bin/x86_64-linux-gcc -O2 -c t.c -o t.o
            '';
          };

          # Tools kbuild runs on the build machine (scripts/, objtool,
          # resolve_btfids, sign-file, pahole for BTF). The target compiler and
          # binutils come from kgcc, which ships binutils 2.41 -- the same
          # version the WSL kernel records in CONFIG_LD_VERSION.
          cross = pkgs.pkgsCross.gnu64.stdenv.cc;
          crossCompile = if haveKgcc then "${kgcc}/bin/x86_64-linux-" else cross.targetPrefix;
          kcc = if haveKgcc then "${kgcc}/bin/gcc" else "${crossCompile}gcc";

          kbuildTools = with pkgs; [
            cross
            bc
            bison
            flex
            perl
            python3
            cpio
            kmod
            pahole
            elfutils
            openssl
            zlib
            zstd
            gmp
            libmpc
          ];

          # Kbuild reads ARCH and CROSS_COMPILE from the environment; the
          # Makefile takes KGCC and passes it as CC.
          kbuildEnv = {
            ARCH = "x86_64";
            CROSS_COMPILE = crossCompile;
            KGCC = kcc;
          };

          # The WSL kernel tree prepared as far as an out-of-tree module needs
          # it -- the nix equivalent of build-kernel-headers.sh. vmlinux is
          # built (not just modules_prepare) for the MODVERSIONS CRCs, and kept
          # so the module gets BTF.
          mkKernelDev =
            kernelRelease: hash:
            let
              kernelVersion = builtins.head (lib.splitString "-" kernelRelease);
              config = ./conf + "/kernel-${kernelRelease}.conf";
            in
            pkgs.stdenv.mkDerivation (
              kbuildEnv
              // {
                pname = "wsl-kernel-dev";
                version = kernelVersion;

                src = pkgs.fetchFromGitHub {
                  owner = "microsoft";
                  repo = "WSL2-Linux-Kernel";
                  tag = "linux-msft-wsl-${kernelVersion}";
                  inherit hash;
                };

                nativeBuildInputs = kbuildTools;

                # The kernel sets its own flags; nix's hardening wrappers only
                # get in the way.
                hardeningDisable = [ "all" ];
                enableParallelBuilding = true;

                postPatch = ''
                  patchShebangs scripts tools
                '';

                # olddefconfig rewrites the compiler-derived options
                # (CC_VERSION_TEXT, GCC_VERSION, AS_/LD_VERSION, CC_HAS_*, ...)
                # from the toolchain it finds. With the kernel's own toolchain
                # only these may change, none of which reach a CRC:
                #   - the header comment, which names ARCH;
                #   - PAHOLE_VERSION, which only affects BTF;
                #   - CC_CAN_LINK, lost because kgcc has no libc. It gates
                #     userspace test programs only.
                #   - DEBUG_INFO_COMPRESSED_ZSTD, unavailable because kgcc's
                #     -gz=zstd probe fails. Debug info compression only.
                # Anything else means the CRCs may not match either, so stop
                # here rather than after the vmlinux build.
                configurePhase = ''
                  runHook preConfigure
                  cp ${config} .config
                  make $makeFlags olddefconfig
                  ${lib.optionalString haveKgcc ''
                    if ! diff -u -I '^# Linux/' -I '^CONFIG_PAHOLE_VERSION=' -I '^CONFIG_CC_CAN_LINK=' -I '^# CONFIG_DEBUG_INFO_COMPRESSED_ZSTD is not set$' ${config} .config; then
                      echo "the prepared .config differs from ${config}; the module would not load" >&2
                      exit 1
                    fi
                  ''}
                  runHook postConfigure
                '';

                # LOCALVERSION= keeps setlocalversion from appending "+".
                makeFlags = [
                  "ARCH=x86_64"
                  "CROSS_COMPILE=${crossCompile}"
                  "CC=${kcc}"
                  "LOCALVERSION="
                ];

                buildFlags = [
                  "vmlinux"
                  "modules_prepare"
                ];

                installPhase = ''
                  runHook preInstall

                  cp vmlinux.symvers Module.symvers

                  built_release="$(cat include/config/kernel.release)"
                  if [ "$built_release" != "${kernelRelease}" ]; then
                    echo "prepared tree reports '$built_release', expected '${kernelRelease}'" >&2
                    exit 1
                  fi

                  # Object files are only needed to link vmlinux; drop them to
                  # keep the store path small. scripts/ and tools/ keep theirs.
                  find . -path ./scripts -prune -o -path ./tools -prune -o \
                    \( -name '*.o' -o -name '*.a' -o -name '.*.cmd' \) -type f -print0 \
                    | xargs -0 rm -f
                  rm -rf .git .tmp_vmlinux* vmlinux.unstripped

                  # Module BTF is generated against vmlinux's .BTF section, which
                  # --strip-debug leaves alone; the DWARF is most of its size.
                  ${crossCompile}objcopy --strip-debug vmlinux

                  mkdir -p $out
                  cp -a . $out/

                  runHook postInstall
                '';

                dontFixup = true;
              }
            );

          mkDxgdrm =
            kernelRelease: kernelDev:
            pkgs.stdenv.mkDerivation (
              kbuildEnv
              // {
                pname = "dxgdrm";
                version = "0-${kernelRelease}";
                src = lib.cleanSource self;

                hardeningDisable = [ "all" ];

                KDIR = "${kernelDev}";

                buildPhase = ''
                  runHook preBuild
                  make all
                  runHook postBuild
                '';

                nativeBuildInputs = kbuildTools ++ [ pkgs.removeReferencesTo ];

                # The module carries paths into the prepared kernel tree: in its
                # debug info, and as __FILE__ strings from WARN()s in inline
                # kernel headers. Left in, Nix counts the whole tree (and kgcc
                # and gcc through it) as a runtime dependency of the module --
                # 2 GB of closure in the NixOS image. The debug info goes
                # (modprobe does not need it; .BTF stays), the strings get
                # their store hash blanked, and disallowedReferences keeps it
                # that way.
                installPhase = ''
                  runHook preInstall
                  ${crossCompile}objcopy --strip-debug dxgdrm.ko
                  remove-references-to -t ${kernelDev} dxgdrm.ko
                  install -Dm644 dxgdrm.ko $out/lib/modules/${kernelRelease}/extra/dxgdrm.ko
                  install -Dm644 99-dxgdrm.rules $out/lib/udev/rules.d/99-dxgdrm.rules
                  runHook postInstall
                '';

                disallowedReferences = [ kernelDev ] ++ lib.optional haveKgcc kgcc;

                dontFixup = true;
              }
            );

          kernelDevs = lib.mapAttrs mkKernelDev kernels;
          modules = lib.mapAttrs mkDxgdrm kernelDevs;

          # One package for every supported kernel, laid out as
          # lib/modules/<release>/extra/dxgdrm.ko. The loader picks the one
          # matching `uname -r`.
          dxgdrm-all = pkgs.symlinkJoin {
            name = "dxgdrm-all";
            paths = lib.attrValues modules;
          };

          # The newest release, for the dev shell and the unqualified names.
          latest = lib.last (lib.sort (a: b: lib.versionOlder a b) (lib.attrNames kernels));
        in
        {
          packages =
            lib.mapAttrs' (release: drv: lib.nameValuePair "wsl-kernel-dev-${release}" drv) kernelDevs
            // lib.mapAttrs' (release: drv: lib.nameValuePair "dxgdrm-${release}" drv) modules
            // lib.optionalAttrs haveKgcc { inherit kgcc; }
            // {
              inherit dxgdrm-all;
              wsl-kernel-dev = kernelDevs.${latest};
              dxgdrm = modules.${latest};
              default = dxgdrm-all;
            };

          devShell = pkgs.mkShell (
            kbuildEnv
            // {
              packages = kbuildTools ++ [ pkgs.git ];
              hardeningDisable = [ "all" ];

              KDIR = "${kernelDevs.${latest}}";

              shellHook = ''
                echo "kernel tree: $KDIR (${latest})"
                echo "compiler:    $KGCC"
                echo "build with:  make"
              '';
            }
          );
        };
    in
    {
      lib.kernelReleases = lib.attrNames kernels;

      packages = forAllSystems (pkgs: (perSystem pkgs).packages);
      devShells = forAllSystems (pkgs: {
        default = (perSystem pkgs).devShell;
      });
    };
}
