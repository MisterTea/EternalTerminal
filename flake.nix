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
            static ? false,
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
            linuxExtras = lib.optionals (!static && packageSet.stdenv.hostPlatform.isLinux) [
              packageSet.libselinux
              packageSet.libutempter
            ];
          in
          packageSet.stdenv.mkDerivation rec {
            pname = if static then "eternal-terminal-musl-static" else "eternal-terminal";
            version = "6.2.11";
            src = cleanedSource;

            dontDisableStatic = static;
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
            ]
            ++ lib.optionals static [
              "-DBUILD_SHARED_LIBS=OFF"
              "-DCMAKE_DISABLE_FIND_PACKAGE_SELinux=TRUE"
              "-DCMAKE_DISABLE_FIND_PACKAGE_UTempter=TRUE"
              "-DOPENSSL_USE_STATIC_LIBS=TRUE"
              "-DProtobuf_USE_STATIC_LIBS=ON"
              "-Dsodium_USE_STATIC_LIBS=ON"
            ];

            meta = with lib; {
              description =
                if static then
                  "Remote terminal with a musl static build"
                else
                  "Remote terminal that reconnects without interrupting the session";
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
        eternalTerminalStatic =
          if pkgs.stdenv.hostPlatform.isLinux then
            mkEternalTerminal {
              packageSet = pkgs.pkgsStatic;
              buildPackages = pkgs.pkgsStatic.buildPackages;
              static = true;
            }
          else
            null;
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
        staticPackageBuildInputs = [
          pkgs.pkgsStatic."abseil-cpp"
          pkgs.pkgsStatic.libsodium
          pkgs.pkgsStatic.libunwind
          pkgs.pkgsStatic.openssl
          pkgs.pkgsStatic.protobuf
          pkgs.pkgsStatic.xz
          pkgs.pkgsStatic.zlib
        ];
        cmakePrefixPath = lib.makeSearchPathOutput "dev" "" packageBuildInputs;
        cmakePrefixPathStatic = lib.makeSearchPathOutput "dev" "" staticPackageBuildInputs;
      in
      {
        packages = {
          default = eternalTerminal;
          "eternal-terminal" = eternalTerminal;
        }
        // lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
          "portable-musl" = eternalTerminalStatic;
          "static-musl" = eternalTerminalStatic;
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
        }
        // lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
          portable-musl = pkgs.mkShell {
            inputsFrom = [ eternalTerminalStatic ];

            packages = [
              pkgs.curl
              pkgs.git
              pkgs.gnumake
              pkgs.llvmPackages_18.clang-tools
            ];

            shellHook = ''
              export OPENSSL_ROOT_DIR="${pkgs.pkgsStatic.openssl.dev}"
              export CMAKE_PREFIX_PATH="${cmakePrefixPathStatic}:$CMAKE_PREFIX_PATH"
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
