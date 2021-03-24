<p align="center">
  <a href="https://sentry.io" target="_blank" align="center">
    <img src="https://sentry-brand.storage.googleapis.com/sentry-logo-black.png" width="280">
  </a>
  <br />
</p>

# Official Sentry SDK for C/C++ <!-- omit in toc -->

The _Sentry Native SDK_ is an error and crash reporting client for native
applications, optimized for C and C++. Sentry allows to add tags, breadcrumbs
and arbitrary custom context to enrich error reports. Supports Sentry _20.6.0_
and later.

**Note**: This SDK is being actively developed and still in Beta. We recommend
to check for updates regularly to benefit from latest features and bug fixes.
Please see [Known Limitations](#known-limitations).

## Resources <!-- omit in toc -->

- [Discord](https://discord.gg/ez5KZN7) server for project discussions.
- Follow [@getsentry](https://twitter.com/getsentry) on Twitter for updates

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
changelog of every version.

[releases]: https://github.com/getsentry/sentry-native/releases

### What is Inside

The SDK bundle contains the following folders:

- `external`: These are external projects which are consumed via
  `git submodules`.
- `include`: Contains the Sentry header file. Set the include path to this
  directory or copy the header file to your source tree so that it is available
  during the build.
- `src`: Sources of the Sentry SDK required for building.

## Platform and Feature Support

The SDK currently supports and is tested on the following OS/Compiler variations:

- 64bit Linux with GCC 9
- 64bit Linux with clang 9
- 32bit Linux with GCC 7 (cross compiled from 64bit host)
- 64bit Windows with MSVC 2019
- 32bit Windows with MSVC 2017
- macOS Catalina with most recent Compiler toolchain
- Android API29 built by NDK21 toolchain
- Android API16 built by NDK19 toolchain

Additionally, the SDK should support the following platforms, although they are
not automatically tested, so breakage may occur:

- Windows Versions lower than Windows 10 / Windows Server 2016
- Windows builds with the MSYS2 + MinGW + Clang toolchain

The SDK supports different features on the target platform:

- **HTTP Transport** is currently only supported on Windows and platforms that
  have the `curl` library available. On other platforms, library users need to
  implement their own transport, based on the `function transport` API.
- **Crashpad Backend** is currently only supported on Linux, Windows and macOS.
- **Breakpad Backend** is currently only supported on Linux and Windows.

## Building and Installation

The SDK is developed and shipped as a [CMake] project.
CMake will pick an appropriate compiler and buildsystem toolchain automatically
per platform, and can also be configured for cross-compilation.
System-wide installation of the resulting sentry library is also possible via
CMake.

Building the Crashpad Backend requires a `C++14` compatible compiler.

**Build example**:

```sh
# configure the cmake build into the `build` directory, with crashpad (on macOS)
$ cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
# build the project
$ cmake --build build --parallel
# install the resulting artifacts into a specific prefix (use the correct config on windows)
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
see the dedicated [CMake Guide] for details on how to integrate it with gradle
or use it on the command line.

[cmake]: https://cmake.org/cmake/help/latest/
[cmake guide]: https://developer.android.com/ndk/guides/cmake

**MinGW**:

64-bits is the only platform supported for now.
LLVM + Clang are mandatory here : they are required to generate .pdb files, used by Crashpad for the report generation.

For your application to generate the appropriate .pdb output, you need to activate CodeView file format generation on your application target. To do so, update your own CMakeLists.txt with something like `target_compile_options(${yourApplicationTarget} PRIVATE -gcodeview)`.

If you use a MSYS2 environement to compile with MinGW, make sure to :

- Create an environement variable `MINGW_ROOT` (ex : `C:/msys64/mingw64`)
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
[`CMAKE_OSX_ARCHITECTURES`](https://cmake.org/cmake/help/latest/variable/CMAKE_OSX_ARCHITECTURES.html) define, both with the `Xcode` generator as well
as the default generator:

```sh
# using xcode generator:
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

Make sure that MacOSX SDK 11 or later is used. It is possible that this requires
manually overriding the `SDKROOT`:

```sh
$ export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
```

### Compile-Time Options

The following options can be set when running the cmake generator, for example
using `cmake -D BUILD_SHARED_LIBS=OFF ..`.

- `SENTRY_BUILD_SHARED_LIBS` (Default: ON):
  By default, `sentry` is built as a shared library. Setting this option to
  `OFF` will build `sentry` as a static library instead.
  If sentry is used as a subdirectory of another project, the value `BUILD_SHARED_LIBS` will be inherited by default.

- `SENTRY_PIC` (Default: ON):
  By default, `sentry` is built as a position independent library.

- `SENTRY_EXPORT_SYMBOLS` (Default: ON):
  By default, `sentry` exposes all symbols in the dynamic symbol table. You might want to disable it in case the program intends to `dlopen` third-party shared libraries and avoid symbol collisions.

- `SENTRY_BUILD_RUNTIMESTATIC` (Default: OFF):
  Enables linking with the static MSVC runtime. Has no effect if the compiler is not MSVC.

- `SENTRY_LINK_PTHREAD` (Default: ON):
  Links platform threads library like `pthread` on unix targets.

- `SENTRY_BUILD_FORCE32` (Default: OFF):
  Forces cross-compilation from 64-bit host to 32-bit target. Only has an effect on Linux.

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

  For Windows versions below than `6.0` it is also necessary to use XP toolchain
  in case of MSVC compiler (pass `-T v141_xp` to CMake command line).

- `SENTRY_TRANSPORT` (Default: depending on platform):
  Sentry can use different http libraries to send reports to the server.

  - **curl**: This uses the `curl` library for HTTP handling. This requires
    that the development version of the package is available.
  - **winhttp**: This uses the `winhttp` system library, is only supported on
    Windows and is the default there.
  - **none**: Do not build any http transport. This should be used if users
    want to handle uploads themselves

- `SENTRY_BACKEND` (Default: depending on platform):
  Sentry can use different backends depending on platform.

  - **crashpad**: This uses the out-of-process crashpad handler. It is currently
    only supported on Desktop OSs, and used as the default on Windows and macOS.
  - **breakpad**: This uses the in-process breakpad handler. It is currently
    only supported on Desktop OSs, and used as the default on Linux.
  - **inproc**: A small in-process handler which is supported on all platforms,
    and is used as default on Android.
  - **none**: This builds `sentry-native` without a backend, so it does not handle
    crashes at all. It is primarily used for tests.

- `SENTRY_INTEGRATION_QT` (Default: OFF):
  Builds the Qt integration, which turns Qt log messages into breadcrumbs.

- `SENTRY_BREAKPAD_SYSTEM` / `SENTRY_CRASHPAD_SYSTEM` (Default: OFF):
  This instructs the build system to use system-installed breakpad or crashpad
  libraries instead of using the in-tree version. This is generally not recommended
  for crashpad, as sentry uses a patched version that has attachment support.
  This is being worked on upstream as well, and a future version might work with
  an unmodified crashpad version as well.

| Feature    | Windows | macOS | Linux | Android | iOS |
| ---------- | ------- | ----- | ----- | ------- | --- |
| Transports |         |       |       |         |     |
| - curl     |         | ☑     | ☑     | (✓)     |     |
| - winhttp  | ☑       |       |       |         |     |
| - none     | ✓       | ✓     | ✓     | ☑       | ☑   |
|            |         |       |       |         |     |
| Backends   |         |       |       |         |     |
| - inproc   | ✓       | ✓     | ✓     | ☑       |     |
| - crashpad | ☑       | ☑     | ✓     |         |     |
| - breakpad | ✓       | ✓     | ☑     | (✓)     | (✓) |
| - none     | ✓       | ✓     | ✓     | ✓       |     |

Legend:

- ☑ default
- ✓ supported
- unsupported

### Build Targets

- `sentry`: This is the main library and the only default build target.
- `crashpad_handler`: When configured with the `crashpad` backend, this is
  the out of process crash handler, which will need to be installed along with
  the projects executable.
- `sentry_test_unit`: These are the main unit-tests, which are conveniently built
  also by the toplevel makefile.
- `sentry_example`: This is a small example program highlighting the API, which
  can be controlled via command-line parameters, and is also used for
  integration tests.

## Runtime Configuration

A minimal working example looks like this. For a more elaborate example see the [example.c](examples/example.c) file which is also used to run sentries integration tests.

```c
sentry_options_t *options = sentry_options_new();
sentry_options_set_dsn(options, "https://YOUR_KEY@oORG_ID.ingest.sentry.io/PROJECT_ID");
sentry_init(options);

// your application code …

sentry_shutdown();
```

Other important configuration options include:

- `sentry_options_set_database_path`: Sentry needs to persist some cache data across application restarts, especially for proper handling of release health sessions. It is recommended to set an explicit absolute path corresponding to the applications cache directory (equivalent to `AppData/Local` on Windows, and `XDG_CACHE_HOME` on Linux). Sentry should be given its own directory which is not shared with other application data, as the SDK will enumerate and possibly delete files in that directory. An example might be `$XDG_CACHE_HOME/your-app/sentry`.
  When not set explicitly, sentry will create and use the `.sentry-native` directory inside of the current working directory.
- `sentry_options_set_handler_path`: When using the crashpad backend, sentry will look for a `crashpad_handler` executable in the same directory as the running executable. It is recommended to set this as an explicit absolute path based on the applications install location.
- `sentry_options_set_release`: Some features in sentry, including release health, need to have a release version set. This corresponds to the application’s version and needs to be set explicitly. See [Releases](https://docs.sentry.io/product/releases/) for more information.

## Known Limitations

- The crashpad backend currently has no support for notifying the crashing
  process, and can thus not properly terminate sessions or call the registered
  `before_send` hook. It will also lose any events that have been queued for
  sending at time of crash.

## Development

Please see the [contribution guide](./CONTRIBUTING.md).
