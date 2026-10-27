[![Conan Center](https://shields.io/conan/v/sentry-native)](https://conan.io/center/recipes/sentry-native) [![homebrew](https://img.shields.io/homebrew/v/sentry-native)](https://formulae.brew.sh/formula/sentry-native) [![nixpkgs unstable](https://repology.org/badge/version-for-repo/nix_unstable/sentry-native.svg)](https://github.com/NixOS/nixpkgs/blob/nixos-unstable/pkgs/by-name/se/sentry-native/package.nix) [![vcpkg](https://shields.io/vcpkg/v/sentry-native)](https://vcpkg.link/ports/sentry-native)
<p align="center">
  <a href="https://sentry.io/?utm_source=github&utm_medium=logo" target="_blank">
    <picture>
      <source srcset="https://sentry-brand.storage.googleapis.com/sentry-logo-white.png" media="(prefers-color-scheme: dark)" />
      <source srcset="https://sentry-brand.storage.googleapis.com/sentry-logo-black.png" media="(prefers-color-scheme: light), (prefers-color-scheme: no-preference)" />
      <img src="https://sentry-brand.storage.googleapis.com/sentry-logo-black.png" alt="Sentry" width="280">
    </picture>
  </a>
</p>

# Official Sentry SDK for C/C++ <!-- omit in toc -->
[![GH Workflow](https://img.shields.io/github/actions/workflow/status/getsentry/sentry-native/ci.yml?branch=master)](https://github.com/getsentry/sentry-native/actions)
[![codecov](https://codecov.io/gh/getsentry/sentry-native/branch/master/graph/badge.svg)](https://codecov.io/gh/getsentry/sentry-native)

The _Sentry Native SDK_ is an error and crash reporting client for native
applications, optimized for C and C++. Sentry allows adding tags, breadcrumbs, and arbitrary custom context to enrich error reports. Supports Sentry _20.6.0_
and later.

### Note <!-- omit in toc -->

Using the `sentry-native` SDK in a standalone use case is currently an experimental feature. The SDK’s primary function is to fuel our other SDKs, like [`sentry-java`](https://github.com/getsentry/sentry-java) or [`sentry-unreal`](https://github.com/getsentry/sentry-unreal). Support from our side is best effort and we do what we can to respond to issues in a timely fashion, but please understand if we won’t be able to address your issues or feature suggestions.

## Resources <!-- omit in toc -->

- [SDK Documentation](https://docs.sentry.io/platforms/native/)
- [Discord](https://discord.gg/sentry) server for project discussions
- Follow [@sentry](https://x.com/sentry) on X for updates

## Table of Contents <!-- omit in toc -->

- [Downloads](#downloads)
  - [What is Inside](#what-is-inside)
- [Platform and Feature Support](#platform-and-feature-support)
- [Building and Installation](#building-and-installation)
  - [Compile-Time Options](#compile-time-options)
  - [Build Targets](#build-targets)
- [Runtime Configuration](#runtime-configuration)
- [Known Limitations](#known-limitations)
- [Development](#development)

## Downloads

The SDK can be downloaded from the [Releases] page, which also lists the
changelog of every version. We recommend using our release packages, but if you want to use this repo directly, please follow the [contribution guide](./CONTRIBUTING.md) to understand the setup better.

[releases]: https://github.com/getsentry/sentry-native/releases

### What is Inside

The SDK bundle contains the following folders:

- `include`: Contains the Sentry header file. Set the include path to this
  directory or copy the header file to your source tree so that it is available
  during the build.
- `src`: Sources of the Sentry SDK required for building.
- `ndk`: Sources for the Android NDK JNI layer.
- `external`: These are vendored dependencies fetched via git submodules (use `git submodule update --init --recursive` if you use a git clone rather than a release).

## Platform and Feature Support

The SDK currently supports and is tested on the following OS/Compiler variations:

- x64/arm64 Linux with GCC 14
- x64 Linux with GCC 12
- x64 Linux with GCC 9
- x64/arm64 Linux with clang 19
- x86 Linux with GCC 9 (cross compiled from x64 host)
- x86, x64 and arm64 Windows with MSVC 2022
- macOS 13, 14, 15, 26 with respective most recent Apple compiler toolchain and LLVM clang 15 + 18
- Android API35 built by NDK27 toolchain
- Android API16 built by NDK19 toolchain
- PlayStation via [sentry-playstation](https://github.com/getsentry/sentry-playstation). See [PlayStation documentation](https://docs.sentry.io/platforms/playstation/) to get access.
- Xbox via [sentry-xbox](https://github.com/getsentry/sentry-xbox). See the [Xbox documentation](https://docs.sentry.io/platforms/xbox/) for access.
- Nintendo Switch via [sentry-switch](https://github.com/getsentry/sentry-switch). See [Nintendo Switch documentation](https://docs.sentry.io/platforms/nintendo-switch/) to get access.

Additionally, the SDK should support the following platforms, although they are
not automatically tested, so breakage may occur:

- Windows Versions lower than Windows 10 / Windows Server 2016
- Windows builds with the MSYS2 + MinGW + Clang toolchain (which also runs in CI)

The SDK supports different features on the target platform:

- **HTTP Transport** is currently only supported on Windows and platforms that
  have the `curl` library available. On other platforms, library users need to
  implement their own transport, based on the `function transport` API.
- **Crashpad Backend** is currently only supported on Linux, Windows, and macOS.
- **Client-side stackwalking** is currently only supported on Linux, Windows, and macOS.

## Building and Installation

The SDK is developed and shipped as a [CMake] project.
CMake will pick an appropriate compiler and buildsystem toolchain automatically
per platform, and can also be configured for cross-compilation.
System-wide installation of the resulting Sentry library is also possible via
CMake.

The prerequisites for building differ by platform and backend. You will always need `CMake` to build the code. Additionally, when using the `crashpad` backend, `zlib` is required. On Linux and macOS, `libcurl` is a prerequisite. For more details, check out  the [contribution guide](./CONTRIBUTING.md).

Building the Breakpad and Crashpad backends requires a `C++17` compatible compiler.

**Build example**:

```sh
# configure the cmake build into the `build` directory, with crashpad (on macOS)
$ cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
# build the project
$ cmake --build build --parallel
# install the resulting artifacts into a specific prefix (use the correct config on Windows)
$ cmake --install build --prefix install --config RelWithDebInfo
# which will result in the following (on macOS):
$ exa --tree install
install
├── bin
│  └── crashpad_handler
├── include
│  └── sentry.h
└── lib
   ├── cmake
   │  └── sentry
   ├── libsentry.dylib
   └── libsentry.dylib.dSYM
```

Please refer to the CMake Manual for more details.

**Android**:

The CMake project can also be configured to correctly work with the Android NDK,
see the dedicated [CMake Guide] for details on how to integrate it with Gradle
or use it on the command line.

The `ndk` folder provides a Gradle project that adds a Java JNI layer for Android, suitable for accessing the sentry-native SDK from Java. See the [NDK Readme] for more details about this topic.

[cmake]: https://cmake.org/cmake/help/latest/
[cmake guide]: https://developer.android.com/ndk/guides/cmake
[NDK Readme]: ndk/README.md

**MinGW**:

64-bit is the only platform supported for now.
LLVM + Clang are mandatory here : they are required to generate .pdb files, used by Crashpad for the report generation.

For your application to generate the appropriate .pdb output, you need to activate CodeView file format generation on your application target. To do so, update your own CMakeLists.txt with something like `target_compile_options(${yourApplicationTarget} PRIVATE -gcodeview)`.

If you use an MSYS2 environment to compile with MinGW, make sure to :

- Create an environment variable `MINGW_ROOT` (ex : `C:/msys64/mingw64`)
- Run from `mingw64.exe` : `pacman -S --needed - < ./toolchains/msys2-mingw64-pkglist.txt`
- Build as :

```sh
# Configure with Ninja as generator and use the MSYS2 toolchain file
$ cmake -GNinja -Bbuild -H. -DCMAKE_TOOLCHAIN_FILE=toolchains/msys2.cmake
# build with Ninja
$ ninja -C build
```

**MacOS**:

Building universal binaries/libraries is possible out of the box when using the
[`CMAKE_OSX_ARCHITECTURES`](https://cmake.org/cmake/help/latest/variable/CMAKE_OSX_ARCHITECTURES.html) variable, both with the `Xcode` generator as well
as the default generator:

```sh
# using Xcode generator:
$ cmake -B xcodebuild -GXcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
$ xcodebuild build -project xcodebuild/Sentry-Native.xcodeproj
$ lipo -info xcodebuild/Debug/libsentry.dylib
Architectures in the fat file: xcodebuild/Debug/libsentry.dylib are: x86_64 arm64

# using default generator:
$ cmake -B defaultbuild -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
$ cmake --build defaultbuild --parallel
$ lipo -info defaultbuild/libsentry.dylib
Architectures in the fat file: defaultbuild/libsentry.dylib are: x86_64 arm64
```

Ensure that macOS SDK 11 or later is used (we currently only test against versions 13 and above). This may require
specifying the `SDKROOT`:

```sh
$ export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
```

If you build on macOS using _CMake 4_, then you _must_ specify the `SDKROOT`, because 
[CMake 4 defaults to an empty `CMAKE_OSX_SYSROOT`](https://cmake.org/cmake/help/latest/variable/CMAKE_OSX_SYSROOT.html), 
which could lead to inconsistent include paths when CMake tries to gather the `sysroot` later in the build.

### Compile-Time Options

The following options can be set when running the cmake generator, for example
using `cmake -D BUILD_SHARED_LIBS=OFF ..`.

- `SENTRY_BUILD_SHARED_LIBS` (Default: `ON`):
  By default, `sentry` is built as a shared library. Setting this option to
  `OFF` will build `sentry` as a static library instead.
  If sentry is used as a subdirectory of another project, the value `BUILD_SHARED_LIBS` will be inherited by default.

  When using `sentry` as a static library, make sure to `#define SENTRY_BUILD_STATIC 1` before including the sentry header.

- `SENTRY_PIC` (Default: `ON`):
  By default, `sentry` is built as a position-independent library.

- `SENTRY_BUILD_RUNTIMESTATIC` (Default: `OFF`):
  Enables linking with the static MSVC runtime. Has no effect if the compiler is not MSVC.

- `SENTRY_LINK_PTHREAD` (Default: `ON`):
  Links platform threads library like `pthread` on UNIX targets.

- `SENTRY_BUILD_FORCE32` (Default: `OFF`):
  Forces cross-compilation from 64-bit host to 32-bit target. Only affects Linux.

- `CMAKE_SYSTEM_VERSION` (Default: depending on Windows SDK version):
  Sets up a minimal version of Windows where sentry-native can be guaranteed to run.
  Possible values:

  - `5.1` (Windows XP)
  - `5.2` (Windows XP 64-bit / Server 2003 / Server 2003 R2)
  - `6.0` (Windows Vista / Server 2008)
  - `6.1` (Windows 7 / Server 2008 R2)
  - `6.2` (Windows 8.0 / Server 2012)
  - `6.3` (Windows 8.1 / Server 2012 R2)
  - `10` (Windows 10 / Server 2016 / Server 2019)

  For Windows versions below `6.0`, it is also necessary to use the XP toolchain
  in case of MSVC compiler (pass `-T v141_xp` to CMake command line).

- `SENTRY_TRANSPORT` (Default: depending on platform):
  Sentry can use different HTTP libraries to send reports to the server.

  - **curl**: This uses the `curl` library for HTTP handling. This requires
    that the development version of the package is available.
  - **winhttp**: This uses the `winhttp` system library, is only supported on
    Windows and is the default there.
  - **none**: Do not build any HTTP transport. This should be used if users
    want to handle uploads themselves

- `SENTRY_BACKEND` (Default: depending on platform):
  Sentry can use different backends depending on the platform.

  - **crashpad**: This uses the out-of-process crashpad handler. It is currently
    only supported on Desktop OSes and used as the default on Windows, Linux, and macOS.
  - **breakpad**: This uses the in-process breakpad handler. It is currently
    only supported on Desktop OSs.
  - **inproc**: A small in-process handler that is supported on all platforms,
    and is used as a default on Android.
  - **native**: **(Experimental)** An out-of-process crash handler that uses a
    lightweight daemon to monitor the application, generate minidumps, and send
    crash reports. Supports Linux, macOS, and Windows. Compatible with TSAN and
    ASAN sanitizers. This backend is under active development.
  - **none**: This builds `sentry-native` without a backend, so it does not handle
    crashes. It is primarily used for tests.

- `SENTRY_INTEGRATION_QT` (Default: `OFF`):
  Builds the Qt integration, which turns Qt log messages into breadcrumbs.

- `SENTRY_BREAKPAD_SYSTEM` (Default: `OFF`):
  This instructs the build system to use system-installed breakpad libraries instead of the in-tree version.

- `SENTRY_LIBUNWIND_SYSTEM` (Default: `OFF`, only for Linux):
  This instructs the build system to use a system-installed `libunwind` (found via `pkg-config`) instead of the
  vendored copy in `vendor/libunwind`.

  In contrast to the vendored `libunwind` which is always built as a static archive and either linked into the
  resulting shared library or colocated with the other static artifacts, with `SENTRY_LIBUNWIND_SYSTEM=ON`, the library
  type (shared or static) is determined by the host distribution's package. The `SENTRY_BUILD_SHARED_LIBS` option only
  controls how the dependency is exposed to consuming CMake projects, not the library type of the system `libunwind`
  itself. Ensure matching build and target environments when using system packages.

- `SENTRY_TRANSPORT_COMPRESSION` (Default: `OFF`):
  Adds Gzip transport compression. Requires `zlib`.

- `SENTRY_FOLDER` (Default: not defined):
  Sets the sentry-native projects folder name for generators that support project hierarchy (like Microsoft Visual
  Studio). To use this feature, you need to enable hierarchy via [`USE_FOLDERS` property](https://cmake.org/cmake/help/latest/prop_gbl/USE_FOLDERS.html)

- `CRASHPAD_ENABLE_STACKTRACE` (Default: `OFF`):
  This enables client-side stackwalking when using the crashpad backend. Stack unwinding will happen on the client's
  machine, and the result will be submitted to Sentry attached to the generated minidump. **Note that this feature is
  still experimental**.

- `SENTRY_SDK_NAME` (Default: `sentry.native` or `sentry.native.android`):
  Sets the SDK name that should be included in the reported events. If you're overriding this, also define
  the same value using `target_compile_definitions()` on your own targets that include `sentry.h`.

- `SENTRY_HANDLER_STACK_SIZE` (Default: `64`):
  This specifies the size in KiB of the stack reserved for the crash handler (including hooks like `on_crash` and
  `before_send`) on Windows, Linux, and the `inproc` backend in macOS. Reserving the stack is necessary in case of a
  stack-overflow, where the handler could otherwise no longer execute. This parameter allows users to specify their
  target stack size in KiB, because some applications might require a different value from our default. This value can
  be as small as 16KiB (on `crashpad` and `breakpad`) for handlers to work, but we recommend 32KiB as the lower bound
  on 64-bit systems. **The value should be a multiple of the page size**.

- `SENTRY_THREAD_STACK_GUARANTEE_FACTOR` (Default: `10`, only for Windows):
  Defines the factor by which the thread's stack reserve must exceed the specified guarantee for the handler.
  _Example_: if the `SENTRY_HANDLER_STACK_SIZE` is defined as 64KiB, then the thread's stack reserve must at least have
  a size of 640KiB.
- `SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT` (Default: `ON`, only for Windows):
  Ensures that all threads created after the SDK's initialization will be configured to use the
  `SENTRY_HANDLER_STACK_SIZE` as its handler stack guarantee.

  _Note_: assigning this to all threads only works when building the SDK as a shared library. If you build it as a
  static library, and this option is enabled, only the `sentry_init()` thread will have a stack guarantee for the handler
  (other threads must be manually initialized via `sentry_set_thread_stack_guarantee()`).

- `SENTRY_THREAD_STACK_GUARANTEE_VERBOSE_LOG` (Default: `OFF`, only for Windows):
  Adds info-level logs for every successfully set thread stack guarantee. This is `OFF` by default, because depending
  on the number of threads used (by all dependencies), this could flood the logs. But it will be helpful to anyone
  tuning the thread stack guarantee parameters. Warnings and errors in the process of setting thread stack guarantees
  will always be logged.

### Support Matrix

| Feature    | Windows | macOS | Linux | Android | iOS  |
|------------|---------|-------|-------|---------|------|
| Transports |         |       |       |         |      |
| - curl     |         | ☑     | ☑     | (✓)**   |      |
| - winhttp  | ☑       |       |       |         |      |
| - none     | ✓       | ✓     | ✓     | ☑       | ☑    |
|            |         |       |       |         |      |
| Backends   |         |       |       |         |      |
| - crashpad | ☑       | ☑     | ☑     |         |      |
| - breakpad | ✓       | ✓     | ✓     | (✓)*    | (✓)* |
| - inproc   | ✓       | ✓     | ✓     | ☑       |      |
| - none     | ✓       | ✓     | ✓     | ✓       |      |

Legend:

- ☑ default
- ✓ supported
- (✓) supported with limitations
- `*`: `breakpad` on Android and iOS builds and should work according to upstream but is untested.
- `**`: `curl` as a transport works on Android but isn't used in any supported configuration to reduce the size of our artifacts.

In addition to platform support, the "Advanced Usage" section of the SDK docs now [describes the tradeoffs](https://docs.sentry.io/platforms/native/advanced-usage/backend-tradeoffs/) involved in choosing a suitable backend for a particular use case.

### Build Targets

- `sentry`: This is the main library and the only default build target.
- `crashpad_handler`: When configured with the `crashpad` backend, this is
  the out-of-process crash handler, which will need to be installed along with
  the project's executable.
- `sentry_test_unit`: These are the main unit-tests, which are conveniently built
  also by the toplevel makefile.
- `sentry_example`: This is a small example program highlighting the API, which
  can be controlled via command-line parameters, and is also used for
  integration tests.

## Runtime Configuration

A minimal working example looks like this. For a more elaborate example, see the [example.c](examples/example.c) file, which is also used to run sentries integration tests.

```c
sentry_options_t *options = sentry_options_new();
sentry_options_set_dsn(options, "https://YOUR_KEY@oORG_ID.ingest.sentry.io/PROJECT_ID");
sentry_init(options);

// your application code …

sentry_close();
```

Other important configuration options include:

- `sentry_options_set_database_path`: Sentry needs to persist some cache data across application restarts, especially for proper handling of release health sessions. It is recommended to set an explicit absolute path corresponding to the application's cache directory (equivalent to `AppData/Local` on Windows, and `XDG_CACHE_HOME` on Linux). Sentry should be given its own directory, not shared with other application data, because the SDK will enumerate and possibly delete files in that directory. An example might be `$XDG_CACHE_HOME/your-app/sentry`.
  When not explicitly set, Sentry will create and use the `.sentry-native` directory in the current working directory.
- `sentry_options_set_handler_path`: When using the crashpad backend, Sentry will look for a `crashpad_handler` executable in the same directory as the running executable. It is recommended to set this as an explicit absolute path based on the application's install location.
- `sentry_options_set_release`: Some features in Sentry, including release health, need to have a release version set. This corresponds to the application’s version and needs to be set explicitly. See [Releases](https://docs.sentry.io/product/releases/) for more information.

## Known Limitations

- The crashpad backend on macOS currently has no support for notifying the crashing
  process, and can thus not properly terminate sessions or call the registered
  `before_send` or `on_crash` hook. It will also lose any events that have been queued for
  sending at the time of the crash.
- On Windows, the crashpad and native backends support fast-fail crashes, which bypass SEH (Structured
  Exception Handling) primarily for security reasons. `sentry-native` registers a WER (Windows Error
  Reporting) module, which signals the out-of-process crash handler when a fast-fail crash occurs.
  But since this process bypasses SEH, the application's local exception handler is no longer invoked, which
  also means that for these kinds of crashes, `before_send` and `on_crash` will not be invoked before
  sending the crash report, and thus have no effect.

## Benchmarks

The SDK is automatically benchmarked in the CI on every push to the `master` branch. The benchmarks cover the following scenarios:

- **SDK initialization time**: Measures the duration of the `sentry_init()` call, representing the overall initialization time of the SDK.
- **Backend startup time**: A subset of the SDK initialization time, focusing on the time required to initialize the `inproc`, `breakpad`, or `crashpad` backend.

The benchmarks are run on Windows, macOS, and Linux, and the results are published on [GitHub Pages](https://getsentry.github.io/sentry-native/).
If you want to run benchmarks locally, follow the instructions in the [contribution guide](./CONTRIBUTING.md).

## Development

Please see the [contribution guide](./CONTRIBUTING.md).
