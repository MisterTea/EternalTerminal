{
  description = "EternalTerminal development shell, package, and NixOS module";

  inputs = {
    self.submodules = true;
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    let
      nixosModule =
        {
          config,
          lib,
          pkgs,
          ...
        }:
        import ./default.nix {
          inherit config lib pkgs;
          package = self.packages.${pkgs.system}.default;
        };
    in
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
        };
        lib = pkgs.lib;
        cleanedSource = lib.cleanSourceWith {
          src = ./.;
          filter =
            path: type:
            let
              relPath = lib.removePrefix "${toString ./.}/" (toString path);
            in
            lib.cleanSourceFilter path type
            && !(
              relPath == "build"
              || lib.hasPrefix "build/" relPath
              || relPath == "cov_build"
              || lib.hasPrefix "cov_build/" relPath
              || relPath == "result"
              || lib.hasPrefix "result/" relPath
            );
        };
        mkEternalTerminal =
          {
            packageSet,
            buildPackages ? packageSet.buildPackages,
          }:
          let
            baseBuildInputs = [
              packageSet."abseil-cpp"
              packageSet.libsodium
              packageSet.libunwind
              packageSet.openssl
              packageSet.protobuf
              packageSet.xz
              packageSet.zlib
            ];
            linuxExtras = lib.optionals packageSet.stdenv.hostPlatform.isLinux [
              packageSet.libselinux
              packageSet.libutempter
            ];
          in
          packageSet.stdenv.mkDerivation rec {
            pname = "eternal-terminal";
            version = "6.2.11";
            src = cleanedSource;

            strictDeps = true;
            nativeBuildInputs = [
              buildPackages.cmake
              buildPackages.ninja
              buildPackages."pkg-config"
              buildPackages.protobuf
            ];
            buildInputs = baseBuildInputs ++ linuxExtras;

            cmakeFlags = [
              "-DBUILD_TESTING=OFF"
              "-DDISABLE_SENTRY=ON"
              "-DDISABLE_TELEMETRY=ON"
              "-DDISABLE_VCPKG=ON"
              "-DBASH_COMPLETION_COMPLETIONSDIR=${placeholder "out"}/share/bash-completion/completions"
              "-DZSH_COMPLETIONS_DIR=${placeholder "out"}/share/zsh/site-functions"
            ];

            meta = with lib; {
              description = "Remote terminal that reconnects without interrupting the session";
              homepage = "https://mistertea.github.io/EternalTerminal/";
              license = licenses.asl20;
              mainProgram = "et";
              platforms = platforms.unix;
            };
          };
        eternalTerminal = mkEternalTerminal {
          packageSet = pkgs;
          buildPackages = pkgs.buildPackages;
        };
        packageBuildInputs = [
          pkgs."abseil-cpp"
          pkgs.libsodium
          pkgs.libunwind
          pkgs.openssl
          pkgs.protobuf
          pkgs.zlib
        ]
        ++ lib.optionals pkgs.stdenv.hostPlatform.isLinux [
          pkgs.libselinux
          pkgs.libutempter
        ];
        cmakePrefixPath = lib.makeSearchPathOutput "dev" "" packageBuildInputs;
      in
      {
        packages = {
          default = eternalTerminal;
          "eternal-terminal" = eternalTerminal;
        };

        devShells = {
          default = pkgs.mkShell {
            inputsFrom = [ eternalTerminal ];

            packages = [
              pkgs.autoconf
              pkgs.automake
              pkgs."bash-completion"
              pkgs.curl
              pkgs.git
              pkgs.gnumake
              pkgs.libtool
              pkgs.llvmPackages_18.clang-tools
              pkgs.unzip
              pkgs.zip
            ]
            ++ lib.optionals pkgs.stdenv.hostPlatform.isLinux [ pkgs.gdb ];

            shellHook = ''
              export OPENSSL_ROOT_DIR="${pkgs.openssl.dev}"
              export CMAKE_PREFIX_PATH="${cmakePrefixPath}:$CMAKE_PREFIX_PATH"
            '';
          };
        };
      }
    )
    // {
      nixosModules.default = nixosModule;
      nixosModules.eternalTerminal = nixosModule;
    };
}
