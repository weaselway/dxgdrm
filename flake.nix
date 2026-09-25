{
  description = "dxgdrm out-of-tree module dev environment (WSL2 kernel, x86_64)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs =
    { self, nixpkgs }:
    let
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (
          system: f nixpkgs.legacyPackages.${system}
        );

      # Must match a config in ./conf, see BUILD-NOTES.md.
      kernelRelease = "6.18.33.2-microsoft-standard-WSL2";
      kernelVersion = builtins.head (nixpkgs.lib.splitString "-" kernelRelease);

      perSystem =
        pkgs:
        let
          # WSL kernels are x86_64. On an x86_64 host this is still a "cross"
          # compiler, just one that happens to target the same platform.
          kcc = pkgs.pkgsCross.gnu64.stdenv.cc;
          crossCompile = kcc.targetPrefix;

          # Tools kbuild runs on the build machine (scripts/, objtool,
          # resolve_btfids, sign-file, pahole for BTF).
          kbuildTools = with pkgs; [
            kcc
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

          # Everything kbuild needs, as environment. Kbuild reads ARCH and
          # CROSS_COMPILE from the environment; the Makefile takes KGCC.
          kbuildEnv = {
            ARCH = "x86_64";
            CROSS_COMPILE = crossCompile;
            KGCC = "${crossCompile}gcc";
          };

          # The WSL kernel tree prepared as far as an out-of-tree module needs
          # it -- the nix equivalent of build-kernel-headers.sh. vmlinux is
          # built (not just modules_prepare) for the MODVERSIONS CRCs, and kept
          # so the module gets BTF.
          #
          # Built with nixpkgs' gcc, not the gcc 13.2.0 the shipped kernel was,
          # so the CRCs will not match a real WSL kernel: good for checking the
          # module compiles, not for loading it. Ship from the Docker build.
          wsl-kernel-dev = pkgs.stdenv.mkDerivation (
            kbuildEnv
            // {
              pname = "wsl-kernel-dev";
              version = kernelVersion;

              src = pkgs.fetchFromGitHub {
                owner = "microsoft";
                repo = "WSL2-Linux-Kernel";
                tag = "linux-msft-wsl-${kernelVersion}";
                hash = "sha256-5zGJZdKvTijB5guP05DRmeTh1vXgbX76y0qIUwbumX0=";
              };

              nativeBuildInputs = kbuildTools;

              # The kernel sets its own flags; nix's hardening wrappers only
              # get in the way.
              hardeningDisable = [ "all" ];
              enableParallelBuilding = true;

              postPatch = ''
                patchShebangs scripts tools
              '';

              configurePhase = ''
                runHook preConfigure
                cp ${./conf}/kernel-${kernelRelease}.conf .config
                runHook postConfigure
              '';

              # LOCALVERSION= keeps setlocalversion from appending "+".
              makeFlags = [
                "ARCH=x86_64"
                "CROSS_COMPILE=${crossCompile}"
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

          dxgdrm = pkgs.stdenv.mkDerivation (
            kbuildEnv
            // {
              pname = "dxgdrm";
              version = "0";
              src = self;

              nativeBuildInputs = kbuildTools;
              hardeningDisable = [ "all" ];

              KDIR = "${wsl-kernel-dev}";

              buildPhase = ''
                runHook preBuild
                make all
                runHook postBuild
              '';

              installPhase = ''
                runHook preInstall
                install -Dm644 dxgdrm.ko $out/lib/modules/${kernelRelease}/extra/dxgdrm.ko
                runHook postInstall
              '';

              dontFixup = true;
            }
          );
        in
        {
          packages = {
            inherit wsl-kernel-dev dxgdrm;
            default = dxgdrm;
          };

          devShell = pkgs.mkShell (
            kbuildEnv
            // {
              packages = kbuildTools ++ [ pkgs.git ];
              hardeningDisable = [ "all" ];

              KDIR = "${wsl-kernel-dev}";

              shellHook = ''
                echo "kernel tree: $KDIR (${kernelRelease})"
                echo "build with:  make"
              '';
            }
          );
        };
    in
    {
      packages = forAllSystems (pkgs: (perSystem pkgs).packages);
      devShells = forAllSystems (pkgs: {
        default = (perSystem pkgs).devShell;
      });
    };
}
