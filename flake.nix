{
  description = "dxgdrm out-of-tree module for the WSL2 kernel (x86_64)";

  # The kernel trees, kgcc and the module builder come from wsl-kernel, with
  # its own pinned nixpkgs; see WEASELWAY.md.
  inputs = {
    wsl-kernel.url = "github:weaselway/wsl-kernel";
    nixpkgs.follows = "wsl-kernel/nixpkgs";
  };

  outputs =
    {
      self,
      nixpkgs,
      wsl-kernel,
    }:
    let
      inherit (nixpkgs) lib;

      forAllSystems =
        f: lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (system: f nixpkgs.legacyPackages.${system});

      # 6.18.40.1 has no DRM core, 6.18.54.1 and later no KMS helpers. What is missing
      # is built as modules and shipped with dxgdrm, see build-drm-modules.sh.
      # (lib.hasInfix overflows the stack on a file this size.)
      drmModules =
        release:
        let
          config = builtins.readFile (wsl-kernel.lib.kernelConfig release);
          has = re: builtins.length (builtins.split re config) > 1;
        in
        if has "\n# CONFIG_DRM is not set\n" then
          [
            "drivers/video/hdmi.ko"
            "drivers/gpu/drm/drm.ko"
            "drivers/gpu/drm/drm_kms_helper.ko"
          ]
        else if !has "\nCONFIG_DRM_KMS_HELPER=" then
          [ "drivers/gpu/drm/drm_kms_helper.ko" ]
        else
          [ ];

      # The DRM modules and dxgdrm, built in one copy of the kernel tree. The
      # package is a module root of its own: `modprobe -d <package> dxgdrm`
      # loads them in order.
      dxgdrmArgs =
        release:
        let
          drm = drmModules release;
        in
        {
          pname = "dxgdrm";
          version = "0-${release}";
          inherit release;
          patches = lib.optional (drm != [ ]) ./kernel-drm-as-modules.patch;
          preBuild = lib.optionalString (drm != [ ]) ''
            bash ${./build-drm-modules.sh} $makeFlags -j$NIX_BUILD_CORES
          '';
          external = lib.cleanSource self;
          install = drm ++ [ "M/dxgdrm.ko" ];
          postInstall = ''
            install -Dm644 ${./99-dxgdrm.rules} $out/lib/udev/rules.d/99-dxgdrm.rules
          '';
        };

      perSystem =
        pkgs:
        let
          k = wsl-kernel.legacyPackages.${pkgs.stdenv.hostPlatform.system};
          inherit (wsl-kernel.lib) latest releases;

          modules = lib.genAttrs releases (release: k.mkModules (dxgdrmArgs release));

          # One package for every supported kernel, laid out as
          # lib/modules/<release>/extra/dxgdrm.ko. The loader picks the one
          # matching `uname -r`.
          dxgdrm-all = pkgs.symlinkJoin {
            name = "dxgdrm-all";
            paths = lib.attrValues modules;
          };

          # The newest tree with the DRM modules built, for `make` in the dev
          # shell. Built on demand only; nothing ships it.
          devTree = k.mkModules (
            dxgdrmArgs latest
            // {
              pname = "dxgdrm-dev-tree";
              external = null;
              install = drmModules latest;
              postInstall = ''
                mkdir -p $out/tree
                cp -a . $out/tree/
              '';
              disallowedReferences = [ ];
            }
          );
        in
        {
          packages = lib.mapAttrs' (release: drv: lib.nameValuePair "dxgdrm-${release}" drv) modules // {
            inherit dxgdrm-all;
            dxgdrm = modules.${latest};
            default = dxgdrm-all;
          };

          devShell = pkgs.mkShell (
            k.kbuildEnv
            // {
              packages = k.kbuildTools ++ [ pkgs.git ];
              hardeningDisable = [ "all" ];

              KDIR = "${devTree}/tree";

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
      lib.kernelReleases = wsl-kernel.lib.releases;

      packages = forAllSystems (pkgs: (perSystem pkgs).packages);
      devShells = forAllSystems (pkgs: {
        default = (perSystem pkgs).devShell;
      });
    };
}
