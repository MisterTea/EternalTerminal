# EternalTerminal on NixOS

This repo adds Nix support on top of the upstream CMake and Debian-style packaging.

## Changes from base

- `flake.nix` adds:
  - `packages.default` and `packages.eternal-terminal`
  - `devShells.default`
  - `nixosModules.default` and `nixosModules.eternalTerminal`
- `default.nix` adds a NixOS module for running `etserver`
- `CMakeLists.txt` gates Catch2 behind `BUILD_TESTING=ON` so Nix package builds do not pull test-only dependencies

## Nix behavior in this repo

- Flake builds use `DISABLE_VCPKG=ON`, `DISABLE_SENTRY=ON`, `DISABLE_TELEMETRY=ON`, and `BUILD_TESTING=OFF`.
- The flake uses `inputs.self.submodules = true`, so consumers fetch the vendored submodules needed by the build.
- The package source is filtered with `lib.cleanSourceWith` so local `build/`, `cov_build/`, and `result/` directories do not leak into Nix builds.

## Using it with Nix

For a local checkout, use `path:.`:

```bash
nix flake show path:.
nix develop path:.
nix build path:. --no-link
```

`nix develop` gives you a shell with the package inputs and the extra developer tools needed for normal CMake work on NixOS.

## Using it with NixOS: module + package

Common setup: import the NixOS module for the service, and add the package if you also want the client tools in `PATH`.

The module manages the server side:

- generates `/etc/et.cfg`
- starts `etserver` from the selected package
- opens the configured port when `openFirewall = true`

Example flake-based NixOS configuration:

```nix
{
  inputs.et.url = "github:MisterTea/EternalTerminal";

  outputs = { nixpkgs, et, ... }: {
    nixosConfigurations.my-host = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        et.nixosModules.default
        ({ pkgs, ... }: {
          environment.systemPackages = [
            et.packages.${pkgs.stdenv.hostPlatform.system}.default
          ];

          services.eternalTerminal = {
            enable = true;
            openFirewall = true;
            port = 2022;
            settings.Networking.bind_ip = "0.0.0.0";
          };
        })
      ];
    };
  };
}
```

Notes:

- The module runs the server, but it does not add the client tools to `PATH` by itself. Add the package to `environment.systemPackages` if you also want `et`, `htm`, and the other binaries available interactively.
- When imported through the flake, `services.eternalTerminal.package` defaults to the flake package.
- When importing `./default.nix` directly, set `services.eternalTerminal.package` yourself.

## Module options

- `services.eternalTerminal.enable`
- `services.eternalTerminal.package`
- `services.eternalTerminal.port`
- `services.eternalTerminal.openFirewall`
- `services.eternalTerminal.settings`

`services.eternalTerminal.settings` is written as INI sections into `/etc/et.cfg`. `Networking.port` is always taken from `services.eternalTerminal.port`.

## Service checks

```bash
systemctl status eternal-terminal
journalctl -u eternal-terminal -b
```
