{
  description = "Koshka - the fastest cross-platform Bash and POSIX-compatible shell";

  inputs = {
    self.submodules = true;
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      versionLines = nixpkgs.lib.splitString "\n" (builtins.readFile ./src/Common.hpp);
      versionValue = name:
        let
          matches = builtins.filter
            (line: builtins.match "#define[[:space:]]+${name}[[:space:]]+.*" line != null)
            versionLines;
          line = if matches == [] then
            throw "Missing ${name} in src/Common.hpp"
          else
            builtins.head matches;
        in
        builtins.elemAt
          (builtins.match "#define[[:space:]]+${name}[[:space:]]+(.+)" line)
          0;
      versionExtra = nixpkgs.lib.removeSuffix "\""
        (nixpkgs.lib.removePrefix "\"" (versionValue "KOSH_VER_EXTRA"));
      packageVersion = nixpkgs.lib.concatStringsSep "." [
        (versionValue "KOSH_VER_MAJOR")
        (versionValue "KOSH_VER_MINOR")
        (versionValue "KOSH_VER_PATCH")
      ] + nixpkgs.lib.optionalString (versionExtra != "") "-${versionExtra}";

      mkPackage = { pkgs, mode ? "rel" }:
        pkgs.stdenv.mkDerivation {
          pname = "kosh";
          version = packageVersion;

          src = self;

          nativeBuildInputs = with pkgs; [
            clang
            gnumake
            git
          ];

          buildInputs = nixpkgs.lib.optionals pkgs.stdenv.isLinux [
            pkgs.glibc.static
          ];

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
