# Changelog

## 0.4.8

**Features**:

- The unwinder on Android was updated to a newer version.
- Experimental support for the Breakpad backend on Android and iOS.

**Fixes**:

- Fixed some memory leaks on Windows.
- Build System improvements.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@Mixaill](https://github.com/Mixaill)
- [@daxpedda](https://github.com/daxpedda)
- [@Amphaal](https://github.com/Amphaal)

## 0.4.7

**Features**:

- Events will automatically get an `os` context with OS version information.
- Added a new `max_breadcrumbs` option.

**Fixes**:

- Fixed some memory leaks related to bounded breadcrumbs.

## 0.4.6

**Fixes**:

- Restore compatibility with CMake 3.10 (as used in Android NDK Tools)

**Internal**:

- Update Crashpad and Breakpad submodules to 2021-01-25

## 0.4.5

**Features**:

- The Breakpad backend is now supported on macOS, although the crashpad backend is recommended on that platform.
- Added a new `sentry_reinstall_backend` function which can be used in case a third-party library is overriding the signal/exception handler.
- Add a Qt integration that hooks into Qt logging (opt-in CMake option).
- Expose the sentry-native version via CMake.

**Fixes**:

- Install `.pdb` files correctly.
- Improve macOS runtime version detection.
- Fixed a potential segfault when doing concurrent scope modification.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@Mixaill](https://github.com/Mixaill)
- [@eakoli](https://github.com/eakoli)
- [@GenuineAster](https://github.com/GenuineAster)
- [@daxpedda](https://github.com/daxpedda)
- [@torarnv](https://github.com/torarnv)

## 0.4.4

**Features**:

- The `sentry_get_modules_list` function was made public, which will return a list of loaded libraries that will be sent to sentry with each event.
- A new `sentry_options_set_transport_thread_name` function was added to set an explicit name for sentries http transport thread.

**Fixes**:

- The session duration is now printed in a locale-independent way, avoiding invalid session payloads.
- Correctly clean up locks and pass the Windows Application Verifier.
- Build fixes for MinGW and better documentation for universal MacOS builds.
- Crashes captured by the `crashpad` backend _after_ calling `sentry_shutdown` will now have the full metadata.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@Mixaill](https://github.com/Mixaill)

## 0.4.3

**Caution**:

- The representation of `sentry_value_t` was changed to avoid problems with the newly introduced Memory Tagging Extension (MTE) on ARM / Android.
  Implementation details of `sentry_value_t` were never considered public, and it should always be treated as an opaque type.

**Fixes**:

- Fix corrupted breadcrumb data when using the crashpad backend on Windows.
- Avoid sending empty envelopes when using the crashpad backend.
- Correctly encode the signal number when using the Windows inproc backend, avoiding a processing Error.
- Unwind from the local call-stack, fixing empty stacktraces when using the inproc backend on Linux.
- Improvements to the Build configuration.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@4diekmann](https://github.com/4diekmann)
- [@variar](https://github.com/variar)
- [@Mixaill](https://github.com/Mixaill)

## 0.4.2

**Fixes**:

- Sampled (discarded) events still contribute to a sessions `errors` count.
- Initialize all static data structures.

## 0.4.1

**Fixes**:

- Fix parsing rate limit headers with multiple categories.

## 0.4.0

**Breaking Changes**:

- The minimum CMake version required to build on windows was raised to `3.16.4` to avoid potential build failures on older versions.
- The `sentry_get_options` function was removed, as it was unsafe to use after a `sentry_shutdown` call.
- The `sentry_options_set_logger` function now accepts a `userdata` parameter.
- The `name` parameter of `sentry_options_add_attachment(w)` was removed, it will now be inferred from the filename of `path`.
- The transport startup hook that is set via `sentry_transport_set_startup_func` now needs to return an `int`, and a failure will propagate to `sentry_init`.
- The return value of the transport shutdown hook set via `sentry_transport_set_shutdown_func` was also changed to return an `int`.
- Both functions should return _0_ on success, and a non-zero error code on failure, as does `sentry_init`.
- Similarly, the return value of `sentry_shutdown` was also changed to an `int`, and will return _0_ on success and a non-zero error code on unclean shutdown.
- Documentation for custom transports was updated to highlight the ordering requirements of submitted envelopes, which is important for release health.

```c
// before
sentry_options_set_logger(options, my_custom_logger);
sentry_options_add_attachment(options, "some-attachment", "/path/to/some-attachment.txt");

void transport_startup(sentry_options_t *options, void*state) {
}
sentry_transport_set_startup_func(transport, transport_startup);
bool transport_shutdown(uint64_t timeout, void*state) {
  return true;
}
sentry_transport_set_shutdown_func(transport, transport_shutdown);

// after
sentry_options_set_logger(options, my_custom_logger, NULL);
sentry_options_add_attachment(options, "/path/to/some-attachment.txt");

int transport_startup(sentry_options_t *options, void*state) {
  return 0;
}
sentry_transport_set_startup_func(transport, transport_startup);
int transport_shutdown(uint64_t timeout, void*state) {
  return 0;
}
sentry_transport_set_shutdown_func(transport, transport_shutdown);
```

**Features**:

- [Release Health](https://docs.sentry.io/workflow/releases/health/) support is now stable and enabled by default. After the update, you will see the number of crash free sessions and crash free users on the Releases page in Sentry. To disable automatic session tracking, use `sentry_options_set_auto_session_tracking`.
- Breakpad support for Windows. This allows you to use `sentry-native` even on Windows XP! ([#278](https://github.com/getsentry/sentry-native/pull/278))
- Add an in-process backend for Windows. As opposed to Breakpad, stack traces are generated on the device and sent to Sentry for symbolication. ([#287](https://github.com/getsentry/sentry-native/pull/287))
- Support for the Crashpad backend was fixed and enabled for Linux. ([#320](https://github.com/getsentry/sentry-native/pull/320))
- A new `SENTRY_BREAKPAD_SYSTEM` CMake option was added to link to the system-installed breakpad client instead of building it as part of sentry.

**Fixes**:

- Reworked thread synchronization code and logic in `sentry_shutdown`, avoiding an abort in case of an unclean shutdown. ([#323](https://github.com/getsentry/sentry-native/pull/323))
- Similarly, reworked global options handling, avoiding thread safety issues. ([#333](https://github.com/getsentry/sentry-native/pull/333))
- Fixed errors not being properly recorded in sessions. ([#317](https://github.com/getsentry/sentry-native/pull/317))
- Fixed some potential memory leaks and other issues. ([#304](https://github.com/getsentry/sentry-native/pull/304) and others)

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@eakoli](https://github.com/eakoli)
- [@Mixaill](https://github.com/Mixaill)
- [@irov](https://github.com/irov)
- [@jblazquez](https://github.com/jblazquez)
- [@daxpedda](https://github.com/daxpedda)

## 0.3.4

**Fixes**:

- Invalid memory access when `sentry_options_set_debug(1)` is set, leading to an application crash. This bug was introduced in version `0.3.3`. ([#310](https://github.com/getsentry/sentry-native/pull/310)).

## 0.3.3

**Fixes**:

- Fix a memory unsafety issue when calling `sentry_value_remove_by_key`. ([#297](https://github.com/getsentry/sentry-native/pull/297))
- Improvements to internal logging. ([#301](https://github.com/getsentry/sentry-native/pull/301), [#302](https://github.com/getsentry/sentry-native/pull/302))
- Better handling of timeouts. ([#284](https://github.com/getsentry/sentry-native/pull/284))
- Better 32-bit build support. ([#291](https://github.com/getsentry/sentry-native/pull/291))
- Run more checks on CI. ([#299](https://github.com/getsentry/sentry-native/pull/299))

**Thank you**:

Fixes in this release have been contributed by:

- [@eakoli](https://github.com/eakoli)

## 0.3.2

**Features**:

- Implement a new logger hook. ([#267](https://github.com/getsentry/sentry-native/pull/267))

  This adds the new `sentry_options_set_logger` function, which can be used to customize the sentry-internal logging, for example to integrate into an app’s own logging system, or to stream logs to a file.

- New CMake options: `SENTRY_LINK_PTHREAD`, `SENTRY_BUILD_RUNTIMESTATIC` and `SENTRY_EXPORT_SYMBOLS` along with other CMake improvements.

**Fixes**:

- Avoid memory unsafety when loading session from disk. ([#270](https://github.com/getsentry/sentry-native/pull/270))
- Avoid Errors in Crashpad Backend without prior scope changes. ([#272](https://github.com/getsentry/sentry-native/pull/272))
- Fix absolute paths on Windows, and allow using forward-slashes as directory separators. ([#266](https://github.com/getsentry/sentry-native/pull/266), [#289](https://github.com/getsentry/sentry-native/pull/289))
- Various fixes uncovered by static analysis tools, notably excessive allocations by the page-allocator used inside signal handlers.
- Build fixes for MinGW and other compilers.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@Mixaill](https://github.com/Mixaill)
- [@blinkov](https://github.com/blinkov)
- [@eakoli](https://github.com/eakoli)

## 0.3.1

- Add support for on-device symbolication, which is enabled by default on
  Android. Use `sentry_options_set_symbolize_stacktraces` to customize.
- Enable gzip compressed crashpad minidumps on windows.
- Correctly 0-pad short `build-id`s.
- Fix build for 32bit Apple targets.

## 0.3.0

- Always send the newer `x-sentry-envelope` format, which makes this
  incompatible with older on-premise installations.
- Better document and handle non-ASCII paths. Users on windows should use the
  `w` version of the appropriate APIs.
- Avoid segfaults due to failed sentry initialization.
- Avoid creating invalid sessions without a `release`.
- Make `sentry_transport_t` opaque, and instead expose APIs to configure it.
  More functionality related to creating custom transports will be exposed in
  future versions.

### Breaking changes

- The `sentry_backend_free` function was removed.
- The `sentry_backend_t` type was removed.
- The `sentry_transport_t` type is now opaque. Use the following new API to
  create a custom transport.

### New API

- `sentry_transport_new`
- `sentry_transport_set_state`
- `sentry_transport_set_free_func`
- `sentry_transport_set_startup_func`
- `sentry_transport_set_shutdown_func`

See `sentry.h` for more documentation.

### Deprecations

- `sentry_new_function_transport` has been deprecated in favor of the new
  transport builder functions.

## 0.2.6

- Avoid crash with invalid crashpad handler path.

## 0.2.5

- Send sessions to the correct sentry endpoint and make sure they work.
- Smaller cleanups.

## 0.2.4

- Avoid unsafe reads in the linux module finder.
- Update to latest crashpad snapshot.
- Yet more CMake improvements (thanks @madebr and @Amphaal).

## 0.2.3

### Important upgrade notice

All `0.2.x` versions prior to this one were affected by a bug that could
potentially lead to serious data-loss on Windows platforms. We encourage
everyone to update as quickly as possible.
See [#220](https://github.com/getsentry/sentry-native/issues/220) for details.

### Deprecations

- `sentry_transport_t` will be replaced by an opaque struct with setter methods
  in a future release.
- `sentry_backend_free` and `sentry_backend_t` are deprecated and will be
  removed in a future release.

### Other changes

- Further improvements to the cmake build system (huge thanks to @madebr
  [#207](https://github.com/getsentry/sentry-native/pull/207))
- Improved support for older Windows versions, as low as Windows XP SP3 (thanks
  to @Mixaill [#203](https://github.com/getsentry/sentry-native/pull/203),
  @cammm [#202](https://github.com/getsentry/sentry-native/pull/202) and
  @jblazquez [#212](https://github.com/getsentry/sentry-native/pull/212))
- Improved documentation
- Cleaned up sentry database handling
- Added new `sentry_handle_exception` function to explicitly capture a crash
  (thanks @cammm [#201](https://github.com/getsentry/sentry-native/pull/201))
- Added new `sentry_clear_modulecache` function to clear the list of loaded
  modules. Use this function when dynamically loading libraries at runtime.

## 0.2.2

- Implement experimental Session handling
- Implement more fine grained Rate Limiting for HTTP requests
- Implement `sample_rate` option
- In-process and Breakpad backend will not lose events queued for HTTP
  submission on crash
- `sentry_shutdown` will better clean up after itself
- Add Experimental MinGW build support (thanks @Amphaal
  [#189](https://github.com/getsentry/sentry-native/pull/189))
- Various other fixes and improvements

## 0.2.1

- Added Breakpad support on Linux
- Implemented fallback `debug-id` on Linux and Android for modules that are
  built without a `build-id`
- Fixes issues and added CI for more platforms/compilers, including 32-bit Linux
  and 32-bit VS2017
- Further improvements to the CMake configuration (thanks @madebr
  [#168](https://github.com/getsentry/sentry-native/pull/168))
- Added a new `SENTRY_TRANSPORT` CMake option to customize the default HTTP transport

## 0.2.0

- Complete rewrite in C
- Build system was switched to CMake
- Add attachment support
- Better support for custom transports
- The crashpad backend will automatically look for a `crashpad_handler`
  executable next to the running program if no `handler_path` is set.

### Breaking Changes

- The `sentry_uuid_t` struct is now always a `char bytes[16]` instead of a
  platform specific type.
- `sentry_remove_context`: The second parameter was removed.
- `sentry_options_set_transport`:
  This function now takes a pointer to the new `sentry_transport_t` type.
  Migrating from the old API can be done by wrapping with
  `sentry_new_function_transport`, like this:
  ```c
  sentry_options_set_transport(
        options, sentry_new_function_transport(send_envelope_func, &closure_data));
  ```

### Other API Additions

- `size_t sentry_value_refcount(sentry_value_t value)`
- `void sentry_envelope_free(sentry_envelope_t *envelope)`
- `void sentry_backend_free(sentry_backend_t *backend)`

## 0.1.4

- Add an option to enable the system crash reporter
- Fix compilation warnings

## 0.1.3

- Stack unwinding on Android
- Fix UUID generation on Android
- Fix concurrently captured events leaking data in some cases
- Fix crashes when the database path contains both slashes and backslashes
- More robust error handling when creating the database folder
- Fix wrong initialization of CA info for the curl backend
- Disable the system crash handler on macOS for faster crashes

## 0.1.2

- Fix SafeSEH builds on Win32
- Fix a potential error when shutting down after unloading libsentry on macOS

## 0.1.1

- Update Crashpad
- Fix compilation on Windows with VS 2019
- Fix a bug in the JSON serializer causing invalid escapes
- Fix a bug in the Crashpad backend causing invalid events
- Reduce data event data sent along with minidumps
- Experimental support for Android NDK

## 0.1.0

- Support for capturing messages
- Add an API to capture arbitrary contexts (`sentry_set_context`)
- Fix scope information being lost in some cases
- Experimental on-device unwinding support
- Experimental on-device symbolication support

## 0.0.4

- Breakpad builds on all platforms
- Add builds for Windows (x86)
- Add builds for Linux

## 0.0.3

- Fix debug information generation on macOS

## 0.0.2

- Crashpad builds on macOS
- Crashpad builds on Windows (x64)

## 0.0.1

Initial Release
