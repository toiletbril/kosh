{
  description = "Koshka - the fastest cross-platform Bash and POSIX-compatible shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    toiletline = {
      url = "github:toiletbril/toiletline/staging";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, toiletline }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;

      mkPackage = { pkgs, mode ? "rel" }:
        pkgs.stdenv.mkDerivation {
          pname = "kosh";
          version = "0.1.0";

          src = self;

          nativeBuildInputs = with pkgs; [
            clang
            gnumake
            git
          ];

          preBuild = ''
            if [ ! -f src/toiletline/toiletline.h ]; then
              echo "providing toiletline submodule from flake input"
              rm -rf src/toiletline
              cp -r ${toiletline} src/toiletline
            fi
          '';

          buildPhase = ''
            runHook preBuild
            make -C src -j$NIX_BUILD_CORES MODE=${mode} CXX=${pkgs.clang}/bin/clang++
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            cp ./kosh $out/bin/
            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description = "The fastest cross-platform Bash and POSIX-compatible shell";
            homepage = "https://github.com/toiletbril/kosh";
            license = licenses.bsd3;
            mainProgram = "kosh";
            platforms = platforms.unix;
          };
        };

      mkSystemModule = { config, lib, pkgs, ... }:
        let
          cfg = config.programs.kosh;
        in
        {
          options.programs.kosh = {
            enable = lib.mkEnableOption "Koshka - the fastest cross-platform Bash and POSIX-compatible shell";
            package = lib.mkOption {
              type = lib.types.package;
              default = mkPackage { inherit pkgs; };
              description = "The kosh package to use";
            };
          };

          config = lib.mkIf cfg.enable {
            environment.systemPackages = [ cfg.package ];
            environment.shells = [ "${cfg.package}/bin/kosh" ];
          };
        };
    in
    {
      packages = forAllSystems (system: let pkgs = nixpkgs.legacyPackages.${system}; in {
        default = self.packages.${system}.kosh;
        kosh = mkPackage { inherit pkgs; };
      });

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.kosh ];

            packages = with pkgs; [
              clang
              clang-tools
              gnumake
              git
            ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux (with pkgs; [
              mold
              lld
            ]);

            shellHook = ''
              echo "  Build:  make -C src MODE=dbg"
              echo "  Test:   make -C test test"
            '';
          };
        }
      );

      overlays.default = final: prev: {
        kosh = mkPackage { pkgs = final; };
      };

      nixosModules.default = mkSystemModule;

      darwinModules.default = mkSystemModule;

      homeModules.default = { config, lib, pkgs, ... }:
        let
          cfg = config.programs.kosh;
        in
        {
          options.programs.kosh = {
            enable = lib.mkEnableOption "Koshka - the fastest cross-platform Bash and POSIX-compatible shell";
            package = lib.mkOption {
              type = lib.types.package;
              default = mkPackage { inherit pkgs; };
              description = "The kosh package to use";
            };
          };

          config = lib.mkIf cfg.enable {
            home.packages = [ cfg.package ];
          };
        };
    };
}
