# EternalTerminal on NixOS

This file captures the NixOS-specific knowledge gathered while bringing
EternalTerminal up in a Nix development shell, turning that shell into a real
package, and adding a NixOS module.

## What was added

- `flake.nix`
  - dev shell
  - package output
  - NixOS module export
- `default.nix`
  - NixOS module for `etserver`
- `README.md`
  - short NixOS usage snippet
- `CMakeLists.txt`
  - small packaging fix so Catch2 is only required when `BUILD_TESTING=ON`

## Main outcomes

- The repo now has a working Nix dev shell.
- The repo now has a buildable flake package.
- The repo now exports a NixOS module.
- The package build works without vcpkg.
- The package installs the expected binaries and shell completions.
- The module generates `/etc/et.cfg` and runs the packaged `etserver` binary.

## Build facts discovered from the repo

The CMake build depends on these main tools and libraries on Linux:

- `cmake`
- `ninja`
- `pkg-config`
- `openssl`
- `zlib`
- `libsodium`
- `protobuf` and `protoc`
- `libunwind`
- `libutempter`
- `libselinux`

Other tools that were useful in the dev shell:

- `autoconf`
- `automake`
- `libtool`
- `curl`
- `git`
- `zip`
- `unzip`
- `gdb`
- `llvmPackages_18.clang-tools`
- `bash-completion`

Important CMake details:

- `-DDISABLE_VCPKG=ON` is the right path for Nix.
- `-DDISABLE_TELEMETRY=ON` was used for local build verification.
- `-DDISABLE_SENTRY=ON` is used in the package build.
- `protobuf_generate_cpp` is used for `proto/ET.proto` and `proto/ETerminal.proto`.
- Vendored submodules are used for `Catch2`, `cxxopts`, `cpp-httplib`, `json`, and other bundled code when vcpkg is disabled.

## Problems hit during the run

### 1. Untracked flake behavior

Running `nix develop --command ...` against an untracked `flake.nix` inside a
git repo failed.

Working workaround during development:

```bash
nix develop path:. --command <command>
```

Using `path:.` lets Nix treat the repo as a path flake instead of insisting on a
tracked git flake.

### 2. Deprecated clang-tools attribute

`clang-tools_18` is deprecated in current `nixpkgs`.

Correct replacement:

```nix
pkgs.llvmPackages_18.clang-tools
```

### 3. Missing submodule content

Early builds failed because the repo had incomplete submodule checkouts.
Notable breakages:

- missing `external/sanitizers-cmake`
- missing `external/Catch2/CMakeLists.txt`
- missing `external/easyloggingpp/src/easylogging++.h`
- missing `external/ThreadPool/ThreadPool.h`
- missing `external/base64/base64.h`

Useful recovery commands:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

Some submodules also needed their worktrees restored with:

```bash
git -C external/easyloggingpp reset --hard HEAD
git -C external/ThreadPool reset --hard HEAD
git -C external/base64 reset --hard HEAD
```

### 4. Bash completion warning

CMake warned when `bash-completion` was missing from the shell. Adding
`bash-completion` fixed completion install detection.

### 5. Package build pulling in local build artifacts

Using the repo directly as package source let local build outputs leak into the
source tree view. The package now uses `lib.cleanSourceWith` to filter out:

- `build/`
- `cov_build/`
- `result/`

### 6. Catch2 being built in package mode

Even with `BUILD_TESTING=OFF`, `CMakeLists.txt` still pulled in Catch2. That was
slower and unnecessary for package builds. The fix was to gate Catch2 behind
`BUILD_TESTING` in both the vendored and `find_package` paths.

## Current `flake.nix` design

The flake does three jobs.

### Dev shell

The dev shell is meant for local hacking and manual builds.

Key points:

- inherits the package inputs with `inputsFrom = [ eternalTerminal ];`
- adds developer tools like clang-format tooling, git, curl, and gdb
- exports:

```bash
OPENSSL_ROOT_DIR=${pkgs.openssl.dev}
CMAKE_PREFIX_PATH=<dev outputs from package inputs>
```

That shell is enough to run the normal CMake build on NixOS.

### Package output

The package is `packages.default` and `packages.eternal-terminal`.

Key settings:

- `inputs.self.submodules = true`
  - this matters so users of the flake also fetch git submodules
- `src = cleanedSource`
- `strictDeps = true`
- `BUILD_TESTING=OFF`
- `DISABLE_VCPKG=ON`
- `DISABLE_SENTRY=ON`
- `DISABLE_TELEMETRY=ON`
- explicit completion install directories under `$out`

The package build inputs are:

- `abseil-cpp`
- `libsodium`
- `libunwind`
- `openssl`
- `protobuf`
- `zlib`
- `libselinux` on Linux
- `libutempter` on Linux

The native build inputs are:

- `cmake`
- `ninja`
- `pkg-config`
- `protobuf`

### NixOS module export

The flake exports:

- `nixosModules.default`
- `nixosModules.eternalTerminal`

Both point at the same module from `default.nix`.

## Current `default.nix` module design

The module is focused on the server side.

Options:

- `services.eternalTerminal.enable`
- `services.eternalTerminal.package`
- `services.eternalTerminal.port`
- `services.eternalTerminal.openFirewall`
- `services.eternalTerminal.settings`

Behavior:

- generates `/etc/et.cfg` with `pkgs.formats.ini { }`
- merges defaults with `services.eternalTerminal.settings`
- forces `Networking.port` to follow `services.eternalTerminal.port`
- opens the firewall when `openFirewall = true`
- starts systemd service `eternal-terminal`

Systemd service command:

```bash
${cfg.package}/bin/etserver --cfgfile=/etc/et.cfg --logtostdout
```

Why a custom NixOS service was needed:

- the upstream Debian service file hardcodes `/usr/bin/etserver`
- it also hardcodes `/etc/et.cfg`
- Debian packaging also carries maintainer scripts and `/lib/systemd/system` / `/etc`
  installation behavior that is not appropriate to reuse as-is in a NixOS module

Current default INI settings from the module:

```ini
[Debug]
logdirectory = /var/log/eternal-terminal
logsize = 20971520
silent = 0
telemetry = false
verbose = 0
```

The module also sets:

- `Networking.port = cfg.port`

Notes:

- importing the module through the flake gives `services.eternalTerminal.package`
  a sensible default
- importing `./default.nix` directly requires setting `services.eternalTerminal.package`

## Why the module does not reuse upstream service packaging directly

Upstream packaging in CMake and Debian is install-oriented for classic systems:

- installs binaries under `/usr/bin` or `/usr/local/bin`
- installs shell completions into system directories
- packages `systemctl/et.service` into `/lib/systemd/system`
- packages `etc/et.cfg` into `/etc`

That is fine for `.deb` packaging, but not for a NixOS module. The NixOS module
instead:

- uses the Nix store path for `etserver`
- generates the config file from module options
- lets NixOS own the systemd unit declaration

## Install behavior discovered from upstream CMake

On non-Windows systems, upstream CMake installs these binaries:

- `et`
- `etserver`
- `etterminal`
- `htm`
- `htmd`

Upstream also installs:

- bash completion as `et`
- zsh completion as `_et`

Those completions only target the `et` client command.

## Commands that worked during the run

### Local dev shell build

```bash
nix develop path:. --command bash -lc 'cmake -S . -B build -GNinja -DDISABLE_VCPKG=ON -DDISABLE_TELEMETRY=ON && cmake --build build -j"$(nproc)"'
```

### Flake output inspection

```bash
nix flake show path:.
```

### Package build

```bash
nix build path:. --no-link
```

### Portable musl package build

```bash
nix build path:.#portable-musl -L
```

This produces a musl/static-flavored package build through the flake package
output instead of a local CMake tree.

### Manual musl development shell

```bash
nix develop path:.#portable-musl
```

This shell is meant for fast local iteration on the musl target without waiting
for a full Nix package rebuild each time.

### Manual musl configure and build

```bash
rm -rf build-musl
cmake -S . -B build-musl -GNinja \
  -DDISABLE_VCPKG=ON \
  -DDISABLE_TELEMETRY=ON \
  -DDISABLE_SENTRY=ON \
  -DBUILD_TESTING=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_SELinux=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_UTempter=TRUE \
  -DOPENSSL_USE_STATIC_LIBS=TRUE \
  -DProtobuf_USE_STATIC_LIBS=ON \
  -Dsodium_USE_STATIC_LIBS=ON
cmake --build build-musl -j"$(nproc)" --target et etserver
```

For repeated tries after a source change, use the incremental build command:

```bash
cmake --build build-musl -j"$(nproc)" --target et etserver
```

To print full linker commands during debugging:

```bash
ninja -C build-musl -v et etserver
```

### Rebuild and test in the dev shell

```bash
nix develop path:. --command bash -lc 'cmake -S . -B build -GNinja -DDISABLE_VCPKG=ON -DDISABLE_TELEMETRY=ON && cmake --build build -j"$(nproc)" && ctest --test-dir build --parallel "$(nproc)"'
```

## Verification completed in this run

The following checks succeeded.

### Flake evaluation

- `nix flake show path:.` showed:
  - `devShells.default`
  - `packages.default`
  - `packages.eternal-terminal`
  - `nixosModules.default`
  - `nixosModules.eternalTerminal`

### Module shape

- `default.nix` evaluates as a module function

### Package build

- `nix build path:. --no-link` produced a package successfully
- `nix build path:.#portable-musl -L` produced the musl/static package successfully

Installed binaries verified in the built output:

- `et`
- `etserver`
- `etterminal`
- `htm`
- `htmd`

Installed completions verified in the built output:

- bash: `share/bash-completion/completions/et`
- zsh: `share/zsh/site-functions/_et`

### Manual musl build

- `nix develop path:.#portable-musl` opened a working musl-oriented dev shell
- local incremental build succeeded with `ninja et etserver` in `build-musl/`

### Test run

- `ctest` passed
- result: `110/110` tests passed

## Warnings seen but not blocking

The codebase still emits warnings during builds.

Notable ones:

- `easylogging++` `wcstombs` warning with glibc fortify
- ignored return value warnings in `DaemonCreator.cpp`
- ignored return value warnings in `TerminalHandler.cpp`

There was also repeated `pkg-config` chatter about `libsepol` while probing
`libselinux`, but CMake still found SELinux and continued.

## Current recommended NixOS usage

Example flake-based NixOS integration:

```nix
{
  inputs.et.url = "github:MisterTea/EternalTerminal";

  outputs = { nixpkgs, et, ... }: {
    nixosConfigurations.my-host = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        et.nixosModules.default
        ({ ... }: {
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

After activation, manage it like any other NixOS service.

Useful checks:

```bash
systemctl status eternal-terminal
which et
```

## Maintainer notes

- Keep `inputs.self.submodules = true` in the flake.
- Keep using `path:.` for local one-off flake commands while files are untracked.
- Keep the package source filtered so local build outputs do not contaminate Nix builds.
- Keep Catch2 gated behind `BUILD_TESTING` so package builds stay smaller and faster.
- Keep `devShells.portable-musl` aligned with the `packages.portable-musl` inputs so
  local musl debugging matches the flake build.
- The musl/static path needs explicit protobuf, absl, utf8_range, and libunwind static
  link handling in CMake.
- If a future package build fails on missing vendored code, check submodule state first.
- If a future CMake configure warns about missing completion directories, check that
  `bash-completion` is present and the completion install dirs are still passed in
  `cmakeFlags`.

## Current working tree note

At the end of this run, the Nix-related working tree changes were:

- modified `CMakeLists.txt`
- new `flake.nix`
- new `default.nix`
- generated `flake.lock`
- updated `README.md`

This file was added afterward to capture the run.
