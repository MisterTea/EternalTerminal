# Changelog

## 0.15.3

**Features**

- Android: Expose app-hang detection through the NDK bindings via `NdkOptions.setEnableAppHangTracking()` and `NdkOptions.setAppHangTimeoutMillis()`. ([#1823](https://github.com/getsentry/sentry-native/pull/1823))
- Windows: add wide-char versions of `sentry_capture_minidump` and `sentry_capture_minidump_n`. ([#1827](https://github.com/getsentry/sentry-native/pull/1827))
- Add `sentry_update_context` to allow updating already set context objects with new key-value pairs, as well as new values for existing keys. ([#1835](https://github.com/getsentry/sentry-native/pull/1835))

**Fixes**:

- Native/Windows: ensure valid event IDs for fast-fail crash envelopes to fix launching of the external crash reporter for fast-fail crashes. ([#1832](https://github.com/getsentry/sentry-native/pull/1832))
- Native/Linux: resolve symbols from split-debug files on Linux. ([#1836](https://github.com/getsentry/sentry-native/pull/1836))

## 0.15.2

**Fixes**:

- Native/Windows: Resolve correct symbol names for crashes in multi-module apps ([#1811](https://github.com/getsentry/sentry-native/pull/1811))
- Crashpad: Resolve correct symbol names for crashes in multi-module apps ([#1813](https://github.com/getsentry/sentry-native/pull/1813), [crashpad#156](https://github.com/getsentry/crashpad/pull/156))

**Features**:

- Added an in-process app-hang detection. When enabled via `sentry_options_set_enable_app_hang_tracking`, a background thread monitors the application and captures an app-hang event if no heartbeat is received within `app_hang_timeout` (default `5000` ms). Call `sentry_app_hang_heartbeat()` regularly from the thread you want monitored. ([#1806](https://github.com/getsentry/sentry-native/pull/1806))

## 0.15.1

**Fixes**:

- Report on partial disk writes when streaming envelopes to file, which previously left truncated envelopes on disk and reported success. ([#1804](https://github.com/getsentry/sentry-native/pull/1804))
- Android: breadcrumb `data` is now sent as a structured object instead of a raw JSON string. ([#1808](https://github.com/getsentry/sentry-native/pull/1808))

**Internal**:

- Refactor envelope writers to better support failure tracking on each layer and not push check responsibility to client code. ([#1807](https://github.com/getsentry/sentry-native/pull/1807))

## 0.15.0

**Breaking**:

- `sentry_value_incref` now returns `sentry_value_t` and `sentry_value_decref` returns `int` (0 if freed). ([#1763](https://github.com/getsentry/sentry-native/pull/1763))

**Features**:

- Native: add opt-in async crash upload mode so crashed apps can exit early after crash data is captured, while the crash daemon finishes potentially large uploads in the background. ([#1739](https://github.com/getsentry/sentry-native/pull/1739))
- Native/Linux: symbolicate stack frames in the crash daemon. ([#1747](https://github.com/getsentry/sentry-native/pull/1747), [#1764](https://github.com/getsentry/sentry-native/pull/1764))
- Add a `transfer_timeout` option for SDK-managed HTTP transports. ([#1741](https://github.com/getsentry/sentry-native/pull/1741))
- Apple: use `os_sync_wait_on_address` for the level-triggered waitable flag in the batcher on modern macOS(14.4+) and iOS(17.4+). ([#1765](https://github.com/getsentry/sentry-native/pull/1765))
- Native/macOS: add thread names. ([#1766](https://github.com/getsentry/sentry-native/pull/1766))
- Add Upload-Metadata header to TUS requests. ([#1795](https://github.com/getsentry/sentry-native/pull/1795))

**Fixes**:

- Native/macOS: crash reports now include full stack traces for all threads. Previously, non-crashing threads showed only a single frame. ([#1768](https://github.com/getsentry/sentry-native/pull/1768))
- Native/Linux: resolve function names for the crashed thread's stacktrace from on-disk ELF symbol tables in the crash daemon, so the most important thread gets symbolicated without ptrace. ([#1764](https://github.com/getsentry/sentry-native/pull/1764))
- Finish active trace on crash. ([#1667](https://github.com/getsentry/sentry-native/pull/1667))
- Native/macOS: fix module `image_size` computation, which could have caused the symbolicator to misattribute every frame to the lowest-addressed image (typically `dyld` or `libsystem`). ([#1740](https://github.com/getsentry/sentry-native/pull/1740))
- Native: raise `SENTRY_CRASH_MAX_MODULES` from `512` to `2048` so processes that load many shared libraries no longer have their minidump module list truncated, which left frames in unrecorded modules without a `debug_id` and unsymbolicatable.
  ([#1738](https://github.com/getsentry/sentry-native/pull/1738))
- Reject overly deep JSON and msgpack payloads during deserialization. ([#1727](https://github.com/getsentry/sentry-native/pull/1727), [#1748](https://github.com/getsentry/sentry-native/pull/1748))
- Read lengths for variadic fingerprints. ([#1730](https://github.com/getsentry/sentry-native/pull/1730))
- Guard against JSON token allocation overflow on 32-bit platforms. ([#1733](https://github.com/getsentry/sentry-native/pull/1733))
- Windows: fix HTTP rate limit response header parsing. ([#1732](https://github.com/getsentry/sentry-native/pull/1732))
- POSIX: prevent condition-variable timeout overflow from busy-spinning flush and shutdown waits. ([#1731](https://github.com/getsentry/sentry-native/pull/1731))
- Native/macOS: fix thread stack descriptor. ([#1726](https://github.com/getsentry/sentry-native/pull/1726))
- Native/macOS: honor the `system_crash_reporter_enabled` option. ([#1743](https://github.com/getsentry/sentry-native/pull/1743))
- Cap rate-limit retry-after values at 24 hours to prevent a MITM-provided response from disabling event delivery for the process lifetime. ([#1744](https://github.com/getsentry/sentry-native/pull/1744))
- Fix a shutdown-time use-after-free window in `sentry_close()`. ([#1750](https://github.com/getsentry/sentry-native/pull/1750))
- curl: free duplicate HTTP response headers to avoid potential leaks. ([#1791](https://github.com/getsentry/sentry-native/pull/1791))
- Native: validate ELF header entry sizes. ([#1746](https://github.com/getsentry/sentry-native/pull/1746))
- Native: clamp `module_count` from the shared crash context. ([#1770](https://github.com/getsentry/sentry-native/pull/1770))
- Prevent database cleanup from following symlinks in run and cache directories. ([#1751](https://github.com/getsentry/sentry-native/pull/1751))
- Structured logs: respect printf argument widths when extracting log parameters to avoid stack-data disclosure and corrupted attributes on 32-bit platforms. ([#1752](https://github.com/getsentry/sentry-native/pull/1752))
- Fix TOCTOU races in transaction/span refcounting by switching to the atomic decref return value. ([#1763](https://github.com/getsentry/sentry-native/pull/1763))
- Fix signed-to-unsigned cast in rate-limit parsing to prevent permanent event suppression. ([#1790](https://github.com/getsentry/sentry-native/pull/1790))
- Fix a potential out-of-bounds read when parsing non-NUL-terminated `sentry-trace` headers. ([#1749](https://github.com/getsentry/sentry-native/pull/1749))
- Harden ELF note parsing against overflow and OOB reads. ([#1773](https://github.com/getsentry/sentry-native/pull/1773))
- Fix memory leak in session deserialization on malformed cached files. ([#1789](https://github.com/getsentry/sentry-native/pull/1789))
- Fix division by zero when breadcrumbs are disabled. ([#1767](https://github.com/getsentry/sentry-native/pull/1767))
- Native: escape JSON attachments. ([#1771](https://github.com/getsentry/sentry-native/pull/1771))
- Reject NaN sample rates. ([#1788](https://github.com/getsentry/sentry-native/pull/1788))
- Handle memory allocation failures during JSON serialization to prevent truncated output. ([#1772](https://github.com/getsentry/sentry-native/pull/1772))
- Guard against overflow in string cloning in internal string utilities. ([#1787](https://github.com/getsentry/sentry-native/pull/1787))
- Fix a file descriptor leak in old-run processing. ([#1792](https://github.com/getsentry/sentry-native/pull/1792))

## 0.14.2

**Fixes**:

- Protect CMAKE_SYSTEM_VERSION to avoid empty values when cross-building. ([#1720](https://github.com/getsentry/sentry-native/pull/1720))

## 0.14.1

**Features**:

- Add [strict trace continuation](https://develop.sentry.dev/sdk/foundations/trace-propagation/#strict-trace-continuation) via `sentry_options_set_strict_trace_continuation`. ([#1663](https://github.com/getsentry/sentry-native/pull/1663))
- Auto-populate `event.user.id` with a persistent per-installation UUID when no explicit user ID is set. ([#1661](https://github.com/getsentry/sentry-native/pull/1661))
- Add cache keep modes, including `SENTRY_CACHE_KEEP_ALWAYS` to cache envelopes regardless of upload result. ([#1707](https://github.com/getsentry/sentry-native/pull/1707))
- Crashpad: add error log for oversized envelopes (HTTP 413 Content Too Large). ([#1706](https://github.com/getsentry/sentry-native/pull/1706), [crashpad#155](https://github.com/getsentry/crashpad/pull/155))
- Crashpad: support modifying attachments after `sentry_init` on macOS. ([#1705](https://github.com/getsentry/sentry-native/pull/1705), [crashpad#153](https://github.com/getsentry/crashpad/pull/153))
- Add `cache_dir` envelope header for external crash reporters. ([#1698](https://github.com/getsentry/sentry-native/pull/1698))
- Add `sentry_attachment_set_type` and `SENTRY_ATTACHMENT_TYPE_*` macros for standard Sentry attachment types. ([#1700](https://github.com/getsentry/sentry-native/pull/1700))
  - Deprecate `sentry_options_add_view_hierarchy*` in favor of `sentry_attach_file*` with `sentry_attachment_set_type`.

**Fixes**:

- Native/Breakpad/Windows: fixed capturing abort(). ([#1708](https://github.com/getsentry/sentry-native/pull/1708))
- Native/Windows: capture fast-fail and stack buffer overrun crashes via WER. ([#1710](https://github.com/getsentry/sentry-native/pull/1710))
- Preserve cached minidump refs. ([#1715](https://github.com/getsentry/sentry-native/pull/1715))

## 0.14.0

**Breaking / Important behavior changes**:

- Metrics are enabled by default. This behavior first appeared in `0.13.5` and is now documented as part of the `0.14.0` behavior. Applications that do not want to send metrics must explicitly opt out with `sentry_options_set_enable_metrics(options, false)`. ([#1609](https://github.com/getsentry/sentry-native/pull/1609))
- Structured logs are enabled by default. This behavior first appeared in `0.13.9` and is now documented as part of the `0.14.0` behavior. Applications that do not want to capture structured logs must explicitly opt out with `sentry_options_set_enable_logs(options, false)`. ([#1673](https://github.com/getsentry/sentry-native/pull/1673))

**Fixes**:

- Native/Linux: correct `MD_LINUX_MAPS` stream type (was tagged as `MD_LINUX_AUXV`). ([#1694](https://github.com/getsentry/sentry-native/pull/1694))
- Native/Linux: drop non-ELF mappings (e.g. `/dev/shm/*`, `(deleted)` files) from the minidump module list. ([#1694](https://github.com/getsentry/sentry-native/pull/1694))
- Native/Linux: merge non-contiguous mappings of the same shared library into a single module, and use the offset==0 mapping as `base_of_image`. Fixes duplicate `ld-linux` entries that confused some debuggers (notably Windows LLDB) reading Linux ARM64 minidumps. ([#1694](https://github.com/getsentry/sentry-native/pull/1694))
- Native/Linux: log when `uname()` is blocked (sandbox/seccomp) and fall back to `/proc/sys/kernel/osrelease` for the OS version. ([#1694](https://github.com/getsentry/sentry-native/pull/1694))
- Native/Linux: emit `LinuxAuxv`, `LinuxCpuInfo`, `LinuxLsbRelease`, `LinuxCmdLine`, `LinuxEnviron`, and `LinuxDsoDebug` streams alongside the existing set, matching what Breakpad writes. LLDB needs `LinuxAuxv` and `LinuxDsoDebug` to identify the dynamic loader and enumerate loaded shared libraries; without them, opening a minidump in LLDB on Linux would only recover one frame per thread. ([#1694](https://github.com/getsentry/sentry-native/pull/1694))
- Native/Linux: replay each thread's stack memory descriptor into `MemoryListStream`. Previously stack bytes were only referenced from the per-thread record, so debuggers that look up memory by virtual address (LLDB) could not read the stack and unwinding stopped at frame 0 even when `eh_frame` was available. ([#1694](https://github.com/getsentry/sentry-native/pull/1694))
- Native/macOS: replay each thread's stack memory descriptor into `MemoryListStream` so LLDB can read stack contents (same fix as Linux above). ([#1694](https://github.com/getsentry/sentry-native/pull/1694))

**Features**:

- Native (Linux, macOS): SMART minidump mode now also captures memory referenced by the registers and stack contents of every captured thread, matching the semantics of `MiniDumpWithIndirectlyReferencedMemory` on Windows (already in effect for the native Windows backend). For each pointer that resolves into a writable heap region, ~1 KiB is captured around it; total budget capped at 4 MiB per dump. Heap-allocated structs reachable from the crashing call stack can now be inspected in LLDB / VS Code. ([#1694](https://github.com/getsentry/sentry-native/pull/1694))

## 0.13.9

**Features**:

- Enable structured logs by default; logs are now opt-out via `sentry_options_set_enable_logs(options, false)`. ([#1673](https://github.com/getsentry/sentry-native/pull/1673))
- Crashpad: add macOS support for the `crashpad_wait_for_upload` flag. ([#1679](https://github.com/getsentry/sentry-native/pull/1679), [crashpad#152](https://github.com/getsentry/crashpad/pull/152))
- Add experimental support for large attachment uploads, opt-in via `sentry_options_set_enable_large_attachments`. ([#1545](https://github.com/getsentry/sentry-native/pull/1545))

**Fixes**:

- Fix event ownership (potential double-decref) in sentry_capture_minidump. ([#1669](https://github.com/getsentry/sentry-native/pull/1669))
- Guard against internal stringbuilder append and reserve size overflows. ([#1672](https://github.com/getsentry/sentry-native/pull/1672))
- Preserve attachments added during crash handling ([#1687](https://github.com/getsentry/sentry-native/pull/1687))
- Fix build-time warnings with C++ builds. ([#1671](https://github.com/getsentry/sentry-native/pull/1671))
- Native: respect the `shutdown_timeout` option in the daemon. ([#1691](https://github.com/getsentry/sentry-native/pull/1691))

## 0.13.8

**Features**:

- Enable experimental `native` backend on Xbox. ([#1666](https://github.com/getsentry/sentry-native/pull/1666))
- Cache consent-revoked envelopes to disk when `cache_keep` or `http_retry` is enabled, instead of discarding them. With `http_retry`, the cached envelopes are sent automatically once consent is given. ([#1542](https://github.com/getsentry/sentry-native/pull/1542))
- Linux: support 32-bit ARM. ([#1659](https://github.com/getsentry/sentry-native/issues/1659))
- Crashpad: capture handler process log output to `<run>/crashpad-handler.log`, matching the SDK's `debug` verbosity. ([#1658](https://github.com/getsentry/sentry-native/pull/1658))

**Fixes**:

- Linux: handle `ENOSYS` in `read_safely` to fix empty module list in seccomp-restricted environments. ([#1655](https://github.com/getsentry/sentry-native/pull/1655))
- macOS: avoid stdio deadlock in breakpad exception handler. ([#1656](https://github.com/getsentry/sentry-native/pull/1656))
- Crashpad: build for 32-bit ARM on Linux. ([#1659](https://github.com/getsentry/sentry-native/issues/1659))
- Native: build for 32-bit ARM on Linux. ([#1659](https://github.com/getsentry/sentry-native/issues/1659))
- Inproc: build vendored libunwind for 32-bit ARM on Linux. ([#1659](https://github.com/getsentry/sentry-native/issues/1659))
- Native: build for 64-bit ARM on Linux with musl. ([#1665](https://github.com/getsentry/sentry-native/pull/1665))
- Native/Linux: prevent shared memory leak on crash. ([#1664](https://github.com/getsentry/sentry-native/pull/1664))
- Native: skip scope flush during crash handling. ([#1668](https://github.com/getsentry/sentry-native/pull/1668))

## 0.13.7

**Features**:

- Add `before_screenshot` hook. ([#1641](https://github.com/getsentry/sentry-native/pull/1641))
- Add error log for oversized envelopes (HTTP 413 Content Too Large). ([#1647](https://github.com/getsentry/sentry-native/pull/1647))

**Fixes**:

- Reset client report counters during initialization ([#1632](https://github.com/getsentry/sentry-native/pull/1632))
- macOS: cache VM regions for FP validation in the new unwinder. ([#1634](https://github.com/getsentry/sentry-native/pull/1634))
- Linux: remove dependency on `stdio` in the unwinder pointer validation code to reduce exposure to async-signal-unsafe functions. ([#1637](https://github.com/getsentry/sentry-native/pull/1637))
- macOS: replace sandbox-incompatible IPC primitives (`sem_open`, `shm_open`, `fork`) with sandbox-safe alternatives (`pthread_mutex`, file-backed `mmap`, `posix_spawn`) so the native backend works inside App Sandbox. ([#1644](https://github.com/getsentry/sentry-native/pull/1644))
- Fix minidump UUID byte order (swap GUID fields to match RSDS format) and use `int64` for image sizes to support modules larger than 2 GB. ([#1651](https://github.com/getsentry/sentry-native/pull/1651))

## 0.13.6

**Features**:

- Add WinGDK (Gaming.Desktop.x64) platform support ([#1631](https://github.com/getsentry/sentry-native/pull/1631))
- Track discarded events via client reports. ([#1549](https://github.com/getsentry/sentry-native/pull/1549))
- Android: allow Sentry.NET to preload the NDK integration to install signal handlers before the .NET runtime. ([#1613](https://github.com/getsentry/sentry-native/pull/1613))

**Fixes**:

- Revert `CMP0141` policy that broke debug info settings for downstream consumers. ([#1630](https://github.com/getsentry/sentry-native/pull/1630))

## 0.13.5

**Features**:

- Add HTTP retry with exponential backoff, opt-in via `sentry_options_set_http_retry`. ([#1520](https://github.com/getsentry/sentry-native/pull/1520))
- Store minidump attachments as separate `.dmp` files in the offline cache for direct debugger access. ([#1607](https://github.com/getsentry/sentry-native/pull/1607))
- Enable metrics by default; metrics are now opt-out via `sentry_options_set_enable_metrics(options, false)`. ([#1609](https://github.com/getsentry/sentry-native/pull/1609))

**Fixes**:

- native: Fix crash daemon failing to detect crashes during OOM on Windows. ([#1603](https://github.com/getsentry/sentry-native/issues/1603))
- Fix `WinHttpReceiveResponse` failures being silently ignored, which could cause envelopes to be lost instead of retried or cached. ([#1620](https://github.com/getsentry/sentry-native/pull/1620))
- Fix missing screenshot attachment in native backend envelope. ([#1624](https://github.com/getsentry/sentry-native/pull/1624))
- Replace `googlesource` submodules with GitHub ones. ([#1628](https://github.com/getsentry/sentry-native/pull/1628))
  - This fixes build-time failures where building sentry-native fails with HTTP 403 errors when fetching the zlib and lss libraries from Chromium.

## 0.13.4

**Features**:

- Symbolicate stack frames in crash daemon on Windows. ([#1595](https://github.com/getsentry/sentry-native/pull/1595))
- Add offline caching support to the new experimental `native` backend. ([#1585](https://github.com/getsentry/sentry-native/pull/1585))

**Fixes**:

- Fix `cache_keep` to only cache envelopes when HTTP send fails, instead of unconditionally on restart. ([#1585](https://github.com/getsentry/sentry-native/pull/1585))
- Fix external crash reporter to work with the new experimental `native` backend. ([#1589](https://github.com/getsentry/sentry-native/pull/1589))
- Fix crash daemon premature exit on Windows ([#1600](https://github.com/getsentry/sentry-native/pull/1600))
- native: Fix incorrect stacktraces on Linux by merging ELF segment mappings from `/proc/pid/maps`. Without merging, `base_of_image` pointed to the code segment instead of the real ELF load base, breaking server-side CFI unwinding. ([#1588](https://github.com/getsentry/sentry-native/pull/1588))
- native: Fix single-frame stacktraces on x86_64 and `-fomit-frame-pointer` builds by capturing DWARF-based backtraces (via libunwind) in the signal handler. The daemon now prefers these over FP-based walking, which fails when RBP is used as a general-purpose register. ([#1588](https://github.com/getsentry/sentry-native/pull/1588))
- native: Fix missing thread names on Windows ([#1601](https://github.com/getsentry/sentry-native/pull/1601))

## 0.13.3

**Features**:

- Add `addAttachment` and `clearAttachments` to the NDK `NativeScope` API for managing file and byte attachments via JNI. ([#1584](https://github.com/getsentry/sentry-native/pull/1584))

**Fixes**:

- inproc: only the handling thread cleans up after the crash. ([#1579](https://github.com/getsentry/sentry-native/pull/1579))
- Propagate transport options (`ca_certs`, `proxy`, `user_agent`) and `handler_path` to the native backend crash daemon. Previously, the daemon did not receive SSL certificate or proxy settings from the parent process, causing SSL errors (curl code 60) when uploading crash reports. The daemon also ignored the user-configured handler path, requiring the `sentry-crash` binary to be placed next to the application executable. ([#1573](https://github.com/getsentry/sentry-native/pull/1573))
- Add module header pages to MemoryList and fix exception code in the native backend. ([#1576](https://github.com/getsentry/sentry-native/pull/1576))
- Fix `CHAIN_AT_START` handler strategy crashing on Android when the chained Mono handler resets the signal handler and re-raises. ([#1572](https://github.com/getsentry/sentry-native/pull/1572))

## 0.13.2

**Features**:

- **Experimental**: Add new `native` crash handling backend as an alternative to `crashpad`, `breakpad`, and `inproc`. This backend uses an out-of-process daemon that monitors the application for crashes, generates minidumps, and sends crash reports to Sentry. It supports Linux, macOS, and Windows, and is fully compatible with TSAN and ASAN sanitizers. This feature is experimental and under active development. ([#1433](https://github.com/getsentry/sentry-native/pull/1433))
- Add `sentry_log()` for plain-string log messages. ([#1566](https://github.com/getsentry/sentry-native/pull/1566))
- Add `sentry_set_release()` / `sentry_set_environment()` for updating release and environment after SDK init. ([#1555](https://github.com/getsentry/sentry-native/pull/1555))

**Fixes**:

- Allow null custom log attributes without debug warning. ([#1552](https://github.com/getsentry/sentry-native/pull/1552))
- Introduce level-triggered wait flag for use in the batcher thread for logs and metrics. ([#1558](https://github.com/getsentry/sentry-native/pull/1558))
- Fixed a data race when re-initializing the SDK while logs or metrics are active. ([#1556](https://github.com/getsentry/sentry-native/pull/1556))

## 0.13.1

**Features**:

- Support `SENTRY_SAMPLE_RATE` and `SENTRY_TRACES_SAMPLE_RATE` environment variables. ([#1540](https://github.com/getsentry/sentry-native/pull/1540))

**Fixes**:

- Fix use-after-free on allocation failure when merging scope tags, extra, and contexts into a captured event. ([#1539](https://github.com/getsentry/sentry-native/pull/1539))
- Remove C++ exception ABI symbols (`_Unwind_Resume`, etc.) from vendored `libunwind` build. ([#1544](https://github.com/getsentry/sentry-native/pull/1544))

## 0.13.0

**Breaking**:

- inproc: since we split `inproc` into signal-handler/UEF part and a separate handler thread, `before_send` and `on_crash` could be called from other threads than the one that crashed. While this was never part of the contract, if your code relies on this, it will no longer work. ([#1446](https://github.com/getsentry/sentry-native/pull/1446))
- Android NDK: `SentryNdk.init(NdkOptions)` now throws an `Exception` if init fails (non-zero return code) rather than silently swallowing the error. ([#1430](https://github.com/getsentry/sentry-native/pull/1430))

**Features**:

- Add support for `abort()` in the `inproc` backend on Windows. ([#1446](https://github.com/getsentry/sentry-native/pull/1446))
- Add `beforeBreadcrumb` callback support ([#1534](https://github.com/getsentry/sentry-native/pull/1534))

**Fixes**:

- Make the signal-handler synchronization fully atomic to fix rare race scenarios. ([#1446](https://github.com/getsentry/sentry-native/pull/1446))
- Reintroduce an FP-based stack-walker for macOS that can start from a user context. This also makes `inproc` backend functional again on macOS 13+. ([#1446](https://github.com/getsentry/sentry-native/pull/1446))
- Split the `inproc` signal handler (and UEF on Windows) into a safe handler part and an "unsafe" handler thread. This minimizes exposure to undefined behavior inside the signal handler. ([#1446](https://github.com/getsentry/sentry-native/pull/1446))
- Use a signal-safe address formatter instead of `snprintf()`. ([#1446](https://github.com/getsentry/sentry-native/pull/1446))

**Internal**:

- Introduce PAC tests for `arm64e` on macOS. ([#1446](https://github.com/getsentry/sentry-native/pull/1446))
- For Linux, the SDK now has a vendored "nognu" `libunwind` as the default stack walker and links it statically, but with PIC enabled so it can be used in PIE executables. ([#1446](https://github.com/getsentry/sentry-native/pull/1446))

**Thank you**:

- [hannojg](https://github.com/hannojg)

## 0.12.8

**Fixes**:

- Fix deadlock when re-initializing the SDK while logs or metrics threads are mid-flush. ([#1518](https://github.com/getsentry/sentry-native/pull/1518))

## 0.12.7

**Features**:

- Add new offline caching options to persist envelopes locally: `sentry_options_set_cache_keep`, `sentry_options_set_cache_max_items`, `sentry_options_set_cache_max_size`, and `sentry_options_set_cache_max_age`. ([#1490](https://github.com/getsentry/sentry-native/pull/1490), [#1493](https://github.com/getsentry/sentry-native/pull/1493))

**Fixes**:

- Remove spurious decref in `sentry_capture_user_feedback()` ([#1510](https://github.com/getsentry/sentry-native/pull/1510))
- Prevent double-decref of event in envelope add functions ([#1511](https://github.com/getsentry/sentry-native/pull/1511))

## 0.12.6

**Features**:

- Add support for metrics. It is currently experimental, and one can enable it by setting `sentry_options_set_enable_metrics`. When enabled, you can record a metric using `sentry_metrics_count()`, `sentry_metrics_gauge()`, or `sentry_metrics_distribution()`. Metrics can be filtered by setting the `before_send_metric` hook. ([#1498](https://github.com/getsentry/sentry-native/pull/1498))

## 0.12.5

**Features**:

- Add attachment support to user feedback. ([#1414](https://github.com/getsentry/sentry-native/pull/1414))

**Fixes**:

- Structured logs: avoid modifying custom per-log attributes when merging with scope attributes. ([#1500](https://github.com/getsentry/sentry-native/pull/1500))

## 0.12.4

**Fixes**:

- Crashpad: namespace mpack to avoid ODR violation. ([#1476](https://github.com/getsentry/sentry-native/pull/1476), [crashpad#143](https://github.com/getsentry/crashpad/pull/143))
- Structured logs: stop local attributes overwriting all globally set attributes. They now get merged, and local values overwrite existing global values for the same key. ([#1486](https://github.com/getsentry/sentry-native/pull/1486))

## 0.12.3

**Fixes**:

- Removed the 10-item limit per envelope for non-session data. Sessions are now limited to 100 per envelope, while other items (e.g., attachments) have no limit in amount. ([#1347](https://github.com/getsentry/sentry-native/pull/1347))
- Align the `breakpad` interface changes introduced with [#1083](https://github.com/getsentry/sentry-native/pull/1083) with the corresponding iOS build. ([#1465](https://github.com/getsentry/sentry-native/pull/1465))
- Add structured logs to debug output when `debug` option is set. ([#1466](https://github.com/getsentry/sentry-native/pull/1466))

## 0.12.2

**Features**:

- Add custom attributes API for logs. When `logs_with_attributes` is set to `true`, treats the first `varg` passed into `sentry_logs_X(message,...)` as a `sentry_value_t` object of attributes. ([#1435](https://github.com/getsentry/sentry-native/pull/1435))
- Add runtime API to query user consent requirement. ([#1443](https://github.com/getsentry/sentry-native/pull/1443))
- Add logs flush on `sentry_flush()`. ([#1434](https://github.com/getsentry/sentry-native/pull/1434))
- Add global attributes API. These are added to all `sentry_log_X` calls. ([#1450](https://github.com/getsentry/sentry-native/pull/1450))

## 0.12.1

**Fixes**:

- PS5/Switch compilation regression (`sentry__process_spawn` signature change) ([#1436](https://github.com/getsentry/sentry-native/pull/1436))

## 0.12.0

**Breaking changes**:

- If you use a narrow string path interface (for instance, `sentry_options_set_database_path()`) on _Windows_ rather than one of the wide string variants (`sentry_options_set_database_pathw()`), then the expected encoding is now UTF-8. ([#1413](https://github.com/getsentry/sentry-native/pull/1413))

**Features**:

- Add an option to use the stack pointer as an upper limit for the stack capture range in `crashpad` on Windows. This is useful for targets like Proton/Wine, where one can't rely on the TEB-derived upper bound being correctly maintained by the system, leading to overly large stack captures per thread. ([#1427](https://github.com/getsentry/sentry-native/pull/1427), [crashpad#137](https://github.com/getsentry/crashpad/pull/137))

**Fixes**:

- Add logs flush on crash. This is not available for macOS with the `crashpad` backend. ([#1404](https://github.com/getsentry/sentry-native/pull/1404))
- Make narrow UTF-8 the canonical path encoding on Windows. ([#1413](https://github.com/getsentry/sentry-native/pull/1413))
- Re-add setting thread name for Windows transport. ([#1424](https://github.com/getsentry/sentry-native/pull/1424))
- Fix AOT interop with managed .NET runtimes. ([#1392](https://github.com/getsentry/sentry-native/pull/1392))

**Internal**:

- Add `SENTRY_SDK_VERSION` CMake cache variable to allow downstream SDKs to override the SDK version at configuration time. ([#1417](https://github.com/getsentry/sentry-native/pull/1417))
- Updated `crashpad` to 2025-10-22. ([#1426](https://github.com/getsentry/sentry-native/pull/1426), [crashpad#136](https://github.com/getsentry/crashpad/pull/136), [mini_chromium#4](https://github.com/getsentry/mini_chromium/pull/4)).
- CI: bump Python to `3.12`. ([#1413](https://github.com/getsentry/sentry-native/pull/1413))
- Remove any `MAX_PATH` dependencies in `crashpad` and `breakpad`. ([#1413](https://github.com/getsentry/sentry-native/pull/1413), [breakpad#43](https://github.com/getsentry/breakpad/pull/43), [crashpad#135](https://github.com/getsentry/crashpad/pull/135))

## 0.11.3

**Features**:

- Add support for outgoing W3C traceparent header propagation with the `propagate_traceparent` option. ([#1394](https://github.com/getsentry/sentry-native/pull/1394))
- Add `sentry_options_set_external_crash_reporter_path` to allow specifying an external crash reporter. ([#1303](https://github.com/getsentry/sentry-native/pull/1303))

**Fixes**:

- Use proper SDK name determination for structured logs `sdk.name` attribute. ([#1399](https://github.com/getsentry/sentry-native/pull/1399))
- Serialize `uint64` values as numerical instead of string. ([#1408](https://github.com/getsentry/sentry-native/pull/1408))

## 0.11.2

**Fixes**:

- Windows: Make symbolication and the modulefinder independent of the system ANSI code page. ([#1389](https://github.com/getsentry/sentry-native/pull/1389))

## 0.11.1

**Features**:

- Add support for structured logs. It is currently experimental, and one can enable it by setting `sentry_options_set_enable_logs`. When enabled, you can capture a log using `sentry_log_info()` (or another log level). Logs can be filtered by setting the `before_send_log` hook. ([#1271](https://github.com/getsentry/sentry-native/pull/1271/))

## 0.11.0

**Breaking changes**:

- Add `user_data` parameter to `traces_sampler`. ([#1346](https://github.com/getsentry/sentry-native/pull/1346))

**Fixes**:

- Include `stddef.h` explicitly in `crashpad` since future `libc++` revisions will stop providing this include transitively. ([#1375](https://github.com/getsentry/sentry-native/pull/1375), [crashpad#132](https://github.com/getsentry/crashpad/pull/132))
- Fall back on `JWASM` in the _MinGW_ `crashpad` build only if _no_ `CMAKE_ASM_MASM_COMPILER` has been defined. ([#1375](https://github.com/getsentry/sentry-native/pull/1375), [crashpad#133](https://github.com/getsentry/crashpad/pull/133))
- Prevent `crashpad` from leaking Objective-C ARC compile options into any parent target linkage. ([#1375](https://github.com/getsentry/sentry-native/pull/1375), [crashpad#134](https://github.com/getsentry/crashpad/pull/134))
- Fixed a TOCTOU race between session init/shutdown and event capture. ([#1377](https://github.com/getsentry/sentry-native/pull/1377))
- Make the Windows resource generation aware of config-specific output paths for multi-config generators. ([#1383](https://github.com/getsentry/sentry-native/pull/1383))
- Remove the `ASM` language from the top-level CMake project, as this triggered CMake policy `CMP194` which isn't applicable to the top-level. ([#1384](https://github.com/getsentry/sentry-native/pull/1384))

**Features**:

- Add a configuration to disable logging after a crash has been detected - `sentry_options_set_logger_enabled_when_crashed()`. ([#1371](https://github.com/getsentry/sentry-native/pull/1371))

**Internal**:

- Support downstream Xbox SDK specifying networking initialization mechanism. ([#1359](https://github.com/getsentry/sentry-native/pull/1359))
- Added `crashpad` support infrastructure for the external crash reporter feature. ([#1375](https://github.com/getsentry/sentry-native/pull/1375), [crashpad#131](https://github.com/getsentry/crashpad/pull/131))

**Docs**:

- Document the CMake 4 requirement on macOS `SDKROOT` due to its empty default for `CMAKE_OSX_SYSROOT` in the `README`. ([#1368](https://github.com/getsentry/sentry-native/pull/1368))

**Thank you**:

- [JanFellner](https://github.com/JanFellner)

## 0.10.1

**Internal**:

- Correctly apply dynamic mutex initialization in unit-tests (fixes running unit-tests in downstream console SDKs). ([#1337](https://github.com/getsentry/sentry-native/pull/1337))

## 0.10.0

**Breaking changes**:

- By using transactions as automatic trace boundaries, transactions will, by default, no longer be part of the same singular trace. This is not the case when setting trace boundaries explicitly (`sentry_regenerate_trace()` or `sentry_set_trace()`), which turns off the automatic management of trace boundaries. ([#1270](https://github.com/getsentry/sentry-native/pull/1270))
- Change transaction sampling to be trace-based. This does not affect you when transactions are used for automatic trace boundaries (as described above), since every transaction is part of a new trace. However, if you manage trace boundaries manually (using `sentry_regenerate_trace()`) or run the Native SDK inside a downstream SDK like the Unity SDK, where these SDKs will manage the trace boundaries, for a given `traces_sample_rate`, either all transactions in a trace get sampled or none do with probability equal to that sample rate. ([#1254](https://github.com/getsentry/sentry-native/pull/1254))
- Moved Xbox toolchains to an Xbox-specific repository [sentry-xbox](https://github.com/getsentry/sentry-xbox). You can request access to the repository by following the instructions in [Xbox documentation](https://docs.sentry.io/platforms/xbox/). ([#1329](https://github.com/getsentry/sentry-native/pull/1329))

**Features**:

- Add `sentry_clear_attachments()` to allow clearing all previously added attachments in the global scope. ([#1290](https://github.com/getsentry/sentry-native/pull/1290))
- Automatically set trace boundaries with every transaction. ([#1270](https://github.com/getsentry/sentry-native/pull/1270))
- Provide `sentry_regenerate_trace()` to allow users to set manual trace boundaries. ([#1293](https://github.com/getsentry/sentry-native/pull/1293))
- Add `Dynamic Sampling Context (DSC)` to events. ([#1254](https://github.com/getsentry/sentry-native/pull/1254))
- Add `sentry_value_new_feedback` and `sentry_capture_feedback` to allow capturing [User Feedback](https://develop.sentry.dev/sdk/data-model/envelope-items/#user-feedback). ([#1304](https://github.com/getsentry/sentry-native/pull/1304))
  - Deprecate `sentry_value_new_user_feedback` and `sentry_capture_user_feedback` in favor of the new API.
- Add `sentry_envelope_read_from_file`, `sentry_envelope_get_header`, and `sentry_capture_envelope`. ([#1320](https://github.com/getsentry/sentry-native/pull/1320))
- Add `(u)int64` `sentry_value_t` type. ([#1326](https://github.com/getsentry/sentry-native/pull/1326))

**Meta**:

- Marked deprecated functions with `SENTRY_DEPRECATED(msg)`. ([#1308](https://github.com/getsentry/sentry-native/pull/1308))

**Internal**:

- Crash events from Crashpad now have `event_id` defined similarly to other backends. This makes it possible to associate feedback at the time of crash. ([#1319](https://github.com/getsentry/sentry-native/pull/1319))

## 0.9.1

**Features**:

- The `sentry_attach_file/bytes`, `sentry_scope_attach_file/bytes` (and their wide-string variants), and `sentry_remove_attachment` have been added to modify the list of attachments that are sent along with sentry events after a call to `sentry_init`. ([#1266](https://github.com/getsentry/sentry-native/pull/1266), [#1275](https://github.com/getsentry/sentry-native/pull/1275))
  - NOTE: When using the `crashpad` backend on macOS, the list of attachments that will be added at the time of a hard crash will be frozen at the time of `sentry_init`, and later modifications will not be reflected.
- Add `sentry_attachment_set_content_type` to allow specifying the content type of attachments. ([#1276](https://github.com/getsentry/sentry-native/pull/1276))
- Add `sentry_attachment_set_filename` to allow specifying the filename of attachments displayed in the Sentry WebUI. ([#1285](https://github.com/getsentry/sentry-native/pull/1285))

**Meta**:

- Identify Xbox as a separate SDK name `sentry.native.xbox`. ([#1287](https://github.com/getsentry/sentry-native/pull/1287))

**Internal**:

- Updated `breakpad` to 2025-06-13. ([#1277](https://github.com/getsentry/sentry-native/pull/1277), [breakpad#41](https://github.com/getsentry/breakpad/pull/41))

## 0.9.0

**Breaking changes**:

- Limiting the proguard rules in the NDK package moves the burden of the configuration to its users. Please ensure to [configure proguard](http://proguard.sourceforge.net/manual/examples.html#native) in your project so native methods in your namespace can be symbolicated if they appear in stack traces. ([#1250](https://github.com/getsentry/sentry-native/pull/1250))
- When tags, contexts, and extra data are applied to events, the event data now takes precedence over the scope data as outlined in the [Hub & Scope Refactoring](https://develop.sentry.dev/sdk/miscellaneous/hub_and_scope_refactoring/#how-is-scope-data-applied-to-events) developer document and the linked RFC [code example](https://github.com/getsentry/rfcs/blob/fn/merge-hub-scope/text/0122-sdk-hub-scope-merge.md#applying-scopes). ([#1253](https://github.com/getsentry/sentry-native/pull/1253))

**Features**:

- Provide `before_send_transaction` callback. ([#1236](https://github.com/getsentry/sentry-native/pull/1236))
- Add support for capturing events with local scopes. ([#1248](https://github.com/getsentry/sentry-native/pull/1248))
- Add Windows support for the `crashpad_wait_for_upload` flag. ([#1255](https://github.com/getsentry/sentry-native/pull/1255), [crashpad#126](https://github.com/getsentry/crashpad/pull/126))

**Fixes**:

- Reduce the scope of the proguard rules in the NDK package to local namespaces. ([#1250](https://github.com/getsentry/sentry-native/pull/1250))
- Close the file and return 0 on success when writing raw envelopes. ([#1260](https://github.com/getsentry/sentry-native/pull/1260))
- Fix event tags, contexts, and extra data to take precedence when applying scope data. ([#1253](https://github.com/getsentry/sentry-native/pull/1253))

**Docs**:

- Document convenience PowerShell runners for formatting and tests on Windows. ([#1247](https://github.com/getsentry/sentry-native/pull/1247))

## 0.8.5

**Breaking changes**:

- Use `propagation_context` as the single source of `trace_id` for spans and events. Transactions no longer create a new trace, but inherit the trace from the `propagation_context` created during SDK initialization. This context can be later modified through `sentry_set_trace()` (primarily used by other SDKs). ([#1200](https://github.com/getsentry/sentry-native/pull/1200))

**Features**:

- Add `sentry_value_new_user(id, username, email, ip_address)` function to avoid ambiguous user-context-keys. ([#1228](https://github.com/getsentry/sentry-native/pull/1228))

**Fixes**:

- Remove compile-time check for the `libcurl` feature `AsynchDNS`. ([#1206](https://github.com/getsentry/sentry-native/pull/1206))
- Support musl on Linux. ([#1233](https://github.com/getsentry/sentry-native/pull/1233))

**Thank you**:

- [gregcotten](https://github.com/gregcotten)

## 0.8.4

**Features**:

- Provide an option for downstream SDKs to attach a view hierarchy file. ([#1191](https://github.com/getsentry/sentry-native/pull/1191))

**Fixes**:

- Provide a more defensive automatic thread stack guarantee. ([#1196](https://github.com/getsentry/sentry-native/pull/1196))

## 0.8.3

**Features**:

- Add an option to attach screenshots on Windows to fatal error events. ([#1170](https://github.com/getsentry/sentry-native/pull/1170), [crashpad#123](https://github.com/getsentry/crashpad/pull/123))
- Add an option for `Crashpad` on Linux to delay application shutdown until the upload of the crash report in the `crashpad_handler` is complete. This is useful for deployment in `Docker` or `systemd`, where the life cycle of additional processes is bound by the application life cycle. ([#1153](https://github.com/getsentry/sentry-native/pull/1153), [crashpad#121](https://github.com/getsentry/crashpad/pull/121))
- Expose `traces_sample_rate` option for synchronization with Android SDK. ([#1176](https://github.com/getsentry/sentry-native/pull/1176))

**Thank you**:

- [mwl4](https://github.com/mwl4)
- [jpnurmi](https://github.com/jpnurmi)

## 0.8.2

**Fixes**:

- Provide a mutex-initializer on platforms with no static pthread initializer for recursive mutexes. ([#1113](https://github.com/getsentry/sentry-native/pull/1113))

**Features**:

- Provide Clang-CL support. ([#1161](https://github.com/getsentry/sentry-native/pull/1161), [crashpad#100](https://github.com/getsentry/crashpad/pull/100))
- Enable Crashpad report upload retry-mechanism for all platforms. ([#1152](https://github.com/getsentry/sentry-native/pull/1152), [crashpad#120](https://github.com/getsentry/crashpad/pull/120))

**Thank you**:

[Nerixyz](https://github.com/Nerixyz)

## 0.8.1

**Features**:

- Added `sentry_set_trace()`. The primary use for this is to allow other SDKs to propagate their trace context. This allows Sentry to connect events on all layers. ([#1137](https://github.com/getsentry/sentry-native/pull/1137))

## 0.8.0

**Breaking changes**:

- Return type of `sentry_capture_minidump()` and `sentry_capture_minidump_n()` changed from `void` to `sentry_uuid_t` to retrieve the event-id for a successful minidump upload. ([#1138](https://github.com/getsentry/sentry-native/pull/1138))

**Features**:

- Ensure support for `http_proxy` and `https_proxy` environment variables across all transports. ([#1111](https://github.com/getsentry/sentry-native/pull/1111))

**Fixes**:

- Ensure that `sentry_capture_minidump()` fails if the provided minidump path cannot be attached, instead of sending a crash event without minidump. ([#1138](https://github.com/getsentry/sentry-native/pull/1138))
- Fix Xbox OS name being reported incorrectly. ([#1148](https://github.com/getsentry/sentry-native/pull/1148))

**Thank you**:

[zsd4yr](https://github.com/zsd4yr)

## 0.7.20

**Features**:

- Auto-detect the latest GDK and Windows SDK for the Xbox build. ([#1124](https://github.com/getsentry/sentry-native/pull/1124))
- Enable debug-option by default when running in a debug-build. ([#1128](https://github.com/getsentry/sentry-native/pull/1128))

**Fixes**:

- Allow older toolchains with assemblers that don't support PAC-stripping instructions on `aarch64` to compile `crashpad`. ([#1125](https://github.com/getsentry/sentry-native/pull/1125), [crashpad#118](https://github.com/getsentry/crashpad/pull/118))
- Set default `max_spans` to 1000. ([#1132](https://github.com/getsentry/sentry-native/pull/1132))

## 0.7.19

**Fixes**:

- Fix a build error on older compilers introduced by C++17 support in `0.7.18` for the `crashpad` backend. ([#1118](https://github.com/getsentry/sentry-native/pull/1118), [crashpad#117](https://github.com/getsentry/crashpad/pull/117), [mini_chromium#2](https://github.com/getsentry/mini_chromium/pull/2))

## 0.7.18

**Features**:

- Add support for Xbox Series X/S. ([#1100](https://github.com/getsentry/sentry-native/pull/1100))
- Add option to set debug log level. ([#1107](https://github.com/getsentry/sentry-native/pull/1107))
- Add `traces_sampler`. ([#1108](https://github.com/getsentry/sentry-native/pull/1108))
- Provide support for C++17 compilers when using the `crashpad` backend. ([#1110](https://github.com/getsentry/sentry-native/pull/1110), [crashpad#116](https://github.com/getsentry/crashpad/pull/116), [mini_chromium#1](https://github.com/getsentry/mini_chromium/pull/1))

## 0.7.17

**Features**:

- [NDK] Expose option to set handler strategy. ([#1099](https://github.com/getsentry/sentry-native/pull/1099))
- Add Linux distributions to the OS context. ([#963](https://github.com/getsentry/sentry-native/pull/963))

**Fixes**:

- Add metadata pointer check to prevent crashes when cleaning the `crashpad` database. ([#1102](https://github.com/getsentry/sentry-native/pull/1102), [crashpad#115](https://github.com/getsentry/crashpad/pull/115))

## 0.7.16

**Features**:

- Add SOCKS5 proxy support for macOS and Linux. ([#1063](https://github.com/getsentry/sentry-native/pull/1063))
- Extend performance API with explicit timings. ([#1093](https://github.com/getsentry/sentry-native/pull/1093))

## 0.7.15

**Fixes**:

- Fix DLL versioning for projects that add the Native SDK as a CMake subdirectory. ([#1086](https://github.com/getsentry/sentry-native/pull/1086))

## 0.7.14

**Features**:

- Android NDK: Add `.loadNativeLibraries()` method to allow pre-loading .so files. ([#1082](https://github.com/getsentry/sentry-native/pull/1082))
- Fill the `ucontext_t` field in the `sentry_ucontext_t` `[on_crash|before_send]`-hook parameter on `macOS` from the `breakpad` backend. ([#1083](https://github.com/getsentry/sentry-native/pull/1083), [breakpad#39](https://github.com/getsentry/breakpad/pull/39))

**Thank you**:

[saf-e](https://github.com/saf-e)

## 0.7.13

**Features**:

- Provide version information for non-static Windows binaries. ([#1076](https://github.com/getsentry/sentry-native/pull/1076), [crashpad#110](https://github.com/getsentry/crashpad/pull/110))
- Add an alternative handler strategy to `inproc` to support `.NET` on Linux and `Mono` on Android (specifically, [.NET MAUI](https://github.com/dotnet/android/issues/9055#issuecomment-2261347912)). ([#1027](https://github.com/getsentry/sentry-native/pull/1027))

**Fixes**:

- Correct the timeout specified for the upload-task awaiting `dispatch_semaphore_wait()` when using an HTTP-proxy on macOS. ([#1077](https://github.com/getsentry/sentry-native/pull/1077), [crashpad#111](https://github.com/getsentry/crashpad/pull/111))
- Emit `transaction.data` inside `context.trace.data`. ([#1075](https://github.com/getsentry/sentry-native/pull/1075))

**Thank you**:

[olback](https://github.com/olback)

## 0.7.12

**Features**:

- Add `sentry_capture_minidump()` to capture independently created minidumps. ([#1067](https://github.com/getsentry/sentry-native/pull/1067))

**Fixes**:

- Add breadcrumb ringbuffer to avoid O(n) memmove on adding more than max breadcrumbs. ([#1060](https://github.com/getsentry/sentry-native/pull/1060))

## 0.7.11

**Fixes**:

- Reject invalid trace- and span-ids in context update from header. ([#1046](https://github.com/getsentry/sentry-native/pull/1046))
- Lookup `GetSystemTimePreciseAsFileTime()` at runtime and fall back to `GetSystemTimeAsFileTime()` to allow running on Windows < 8. ([#1051](https://github.com/getsentry/sentry-native/pull/1051))
- Allow for empty DSN to still initialize crash handler. ([#1059](https://github.com/getsentry/sentry-native/pull/1059))

## 0.7.10

**Fixes**:

- Correct the timestamp resolution to microseconds on Windows. ([#1039](https://github.com/getsentry/sentry-native/pull/1039))

**Thank you**:

- [ShawnCZek](https://github.com/ShawnCZek)

## 0.7.9

**Fixes**:

- Check file-writer construction when writing envelope to path. ([#1036](https://github.com/getsentry/sentry-native/pull/1036))

## 0.7.8

**Features**:

- Let the envelope serialization stream directly to the file. ([#1021](https://github.com/getsentry/sentry-native/pull/1021))
- Support 16kb page sizes on Android 15. ([#1028](https://github.com/getsentry/sentry-native/pull/1028))

## 0.7.7

**Fixes**:

- Further clean up of the exported dependency configuration. ([#1013](https://github.com/getsentry/sentry-native/pull/1013), [crashpad#106](https://github.com/getsentry/crashpad/pull/106))
- Clean-up scope flushing synchronization in crashpad-backend. ([#1019](https://github.com/getsentry/sentry-native/pull/1019), [crashpad#109](https://github.com/getsentry/crashpad/pull/109))
- Rectify user-feedback comment parameter guard. ([#1020](https://github.com/getsentry/sentry-native/pull/1020))

**Internal**:

- Updated `crashpad` to 2024-06-11. ([#1014](https://github.com/getsentry/sentry-native/pull/1014), [crashpad#105](https://github.com/getsentry/crashpad/pull/105))

**Thank you**:

- [@JonLiu1993](https://github.com/JonLiu1993)
- [@dg0yt](https://github.com/dg0yt)
- [@stima](https://github.com/stima)

## 0.7.6

**Fixes**:

- Remove remaining build blockers for the `crashpad` backend on Windows ARM64 when using LLVM-MINGW. ([#1003](https://github.com/getsentry/sentry-native/pull/1003), [crashpad#101](https://github.com/getsentry/crashpad/pull/101))
- Ensure `crashpad` targets are included when building as a shared library using our exported CMake config. ([#1007](https://github.com/getsentry/sentry-native/pull/1007))
- Use `find_dependency()` instead of `find_package()` in the exported CMake config. ([#1007](https://github.com/getsentry/sentry-native/pull/1007), [#1008](https://github.com/getsentry/sentry-native/pull/1008), [crashpad#104](https://github.com/getsentry/crashpad/pull/104))

**Thank you**:

- [@past-due](https://github.com/past-due)
- [@podlaszczyk](https://github.com/podlaszczyk)

## 0.7.5

**Features**:

- Change the timestamp resolution to microseconds. ([#995](https://github.com/getsentry/sentry-native/pull/995))

**Internal**:

- (Android) Switch ndk back to `libc++_static`, and hide it from prefab ([#996](https://github.com/getsentry/sentry-native/pull/996))

## 0.7.4

**Fixes**:

- Allow `crashpad` to run under [Epic's Anti-Cheat Client](https://dev.epicgames.com/docs/game-services/anti-cheat/using-anti-cheat#external-crash-dumpers) by deferring the full `crashpad_handler` access rights to the client application until a crash occurred. ([#980](https://github.com/getsentry/sentry-native/pull/980), [crashpad#99](https://github.com/getsentry/crashpad/pull/99))
- Reserve enough stack space on Windows for our handler to run when the stack is exhausted from stack-overflow. ([#982](https://github.com/getsentry/sentry-native/pull/982))
- Only configure a `sigaltstack` in `inproc` if no previous configuration exists on Linux and Android. ([#982](https://github.com/getsentry/sentry-native/pull/982))
- Store transaction `data` in the event property `extra` since the `data` property is discarded by `relay`. ([#986](https://github.com/getsentry/sentry-native/issues/986))

**Docs**:

- Add compile-time flag `SENTRY_TRANSPORT_COMPRESSION` description to the `README.md` file. ([#976](https://github.com/getsentry/sentry-native/pull/976))

**Internal**:

- Move sentry-android-ndk JNI related parts from sentry-java to sentry-native ([#944](https://github.com/getsentry/sentry-native/pull/944))
  This will create a pre-built `io.sentry:sentry-native-ndk` maven artifact, suitable for being consumed by Android apps.

**Thank you**:

- [@AenBleidd](https://github.com/AenBleidd)
- [@kristjanvalur](https://github.com/kristjanvalur)

## 0.7.2

**Features**:

- Add optional Gzip transport compression via build option `SENTRY_TRANSPORT_COMPRESSION`. Requires system `zlib`. ([#954](https://github.com/getsentry/sentry-native/pull/954))
- Enable automatic MIME detection of attachments sent with crash-reports from the `crashpad_handler`. ([#973](https://github.com/getsentry/sentry-native/pull/973), [crashpad#98](https://github.com/getsentry/crashpad/pull/98))

**Fixes**:

- Fix the Linux build when targeting RISC-V. ([#972](https://github.com/getsentry/sentry-native/pull/972))

**Thank you**:

- [@Strive-Sun](https://github.com/Strive-Sun)
- [@jwinarske](https://github.com/jwinarske)

## 0.7.1

**Features**:

- Add user feedback capability to the Native SDK. ([#966](https://github.com/getsentry/sentry-native/pull/966))

**Internal**:

- Remove the `CRASHPAD_WER_ENABLED` build flag. The WER module is now built for all supported Windows targets, and registration is conditional on runtime Windows version checks. ([#950](https://github.com/getsentry/sentry-native/pull/950), [crashpad#96](https://github.com/getsentry/crashpad/pull/96))

**Docs**:

- Add usage of the breadcrumb `data` property to the example. [#951](https://github.com/getsentry/sentry-native/pull/951)

## 0.7.0

**Breaking changes**:

- Make `crashpad` the default backend for Linux. ([#927](https://github.com/getsentry/sentry-native/pull/927))
- Remove build option `SENTRY_CRASHPAD_SYSTEM`. ([#928](https://github.com/getsentry/sentry-native/pull/928))

**Fixes**:

- Maintain `crashpad` client instance during Native SDK lifecycle. ([#910](https://github.com/getsentry/sentry-native/pull/910))
- Specify correct dependencies for CMake client projects using a system-provided breakpad. ([#926](https://github.com/getsentry/sentry-native/pull/926))
- Correct the Windows header include used by `sentry.h`, which fixes the build of [Swift bindings](https://github.com/thebrowsercompany/swift-sentry). ([#935](https://github.com/getsentry/sentry-native/pull/935))

**Internal**:

- Updated `crashpad` to 2023-11-24. ([#912](https://github.com/getsentry/sentry-native/pull/912), [crashpad#91](https://github.com/getsentry/crashpad/pull/91))
- Fixing `crashpad` build for Windows on ARM64. ([#919](https://github.com/getsentry/sentry-native/pull/919), [crashpad#90](https://github.com/getsentry/crashpad/pull/90), [crashpad#92](https://github.com/getsentry/crashpad/pull/92), [crashpad#93](https://github.com/getsentry/crashpad/pull/93), [crashpad#94](https://github.com/getsentry/crashpad/pull/94))
- Remove options memory leak during consent setting. ([#922](https://github.com/getsentry/sentry-native/pull/922))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@compnerd](https://github.com/compnerd)
- [@stima](https://github.com/stima)
- [@hyp](https://github.com/hyp)

## 0.6.7

**Fixes**:

- Disable sigaltstack on Android. ([#901](https://github.com/getsentry/sentry-native/pull/901))
- Prevent stuck crashpad-client on Windows. ([#902](https://github.com/getsentry/sentry-native/pull/902), [crashpad#89](https://github.com/getsentry/crashpad/pull/89))

## 0.6.6

**Fixes**:

- Use a more up-to-date version of `mini_chromium` as a `crashpad` dependency, which fixes a build error on some systems. ([#891](https://github.com/getsentry/sentry-native/pull/891), [crashpad#88](https://github.com/getsentry/crashpad/pull/88))

**Internal**:

- Updated `libunwindstack` to 2023-09-13. ([#884](https://github.com/getsentry/sentry-native/pull/884), [libunwindstack-ndk#8](https://github.com/getsentry/libunwindstack-ndk/pull/8))
- Updated `crashpad` to 2023-09-28. ([#891](https://github.com/getsentry/sentry-native/pull/891), [crashpad#88](https://github.com/getsentry/crashpad/pull/88))
- Updated `breakpad` to 2023-10-02. ([#892](https://github.com/getsentry/sentry-native/pull/892), [breakpad#38](https://github.com/getsentry/breakpad/pull/38))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@sapphonie](https://github.com/sapphonie)

## 0.6.5

**Fixes**:

- Remove deadlock pattern in dynamic sdk-name assignment ([#858](https://github.com/getsentry/sentry-native/pull/858))

## 0.6.4

**Fixes**:

- Crash events are initialized with level `FATAL` ([#852](https://github.com/getsentry/sentry-native/pull/852))
- Fix MSVC compiler error with on non-Unicode systems ([#846](https://github.com/getsentry/sentry-native/pull/846), [crashpad#85](https://github.com/getsentry/crashpad/pull/85))

**Features**:

- crashpad_handler: log `body` if minidump endpoint response is not `OK` ([#851](https://github.com/getsentry/sentry-native/pull/851), [crashpad#87](https://github.com/getsentry/crashpad/pull/87))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@xyz1001](https://github.com/xyz1001)

## 0.6.3

**Features**:

- Disable PC adjustment in the backend for libunwindstack ([#839](https://github.com/getsentry/sentry-native/pull/839))
- Crashpad backend allows inspection and enrichment of the crash event in the on_crash/before_send hooks ([#843](https://github.com/getsentry/sentry-native/pull/843))
- Add http-proxy support to the `crashpad_handler` ([#847](https://github.com/getsentry/sentry-native/pull/847), [crashpad#86](https://github.com/getsentry/crashpad/pull/86))

**Internal**:

- Updated Breakpad backend to 2023-05-03. ([#836](https://github.com/getsentry/sentry-native/pull/836), [breakpad#35](https://github.com/getsentry/breakpad/pull/35))
- Updated Crashpad backend to 2023-05-03. ([#837](https://github.com/getsentry/sentry-native/pull/837), [crashpad#82](https://github.com/getsentry/crashpad/pull/82))

## 0.6.2

**Features**:

- Extend API with ptr/len-string interfaces. ([#827](https://github.com/getsentry/sentry-native/pull/827))
- Allow setting sdk_name at runtime ([#834](https://github.com/getsentry/sentry-native/pull/834))

## 0.6.1

**Fixes**:

- Remove OpenSSL as direct dependency for the crashpad backend on Linux. ([#812](https://github.com/getsentry/sentry-native/pull/812), [crashpad#81](https://github.com/getsentry/crashpad/pull/81))
- Check `libcurl` for feature `AsynchDNS` at compile- and runtime. ([#813](https://github.com/getsentry/sentry-native/pull/813))
- Allow setting `CRASHPAD_WER_ENABLED` when using system crashpad. ([#816](https://github.com/getsentry/sentry-native/pull/816))

**Docs**:

- Add badges for conan, nix and vcpkg package-repos to README. ([#795](https://github.com/getsentry/sentry-native/pull/795))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@Cyriuz](https://github.com/Cyriuz)
- [@MartinDelille](https://github.com/MartinDelille)

## 0.6.0

**Breaking changes**:

- When built as a shared library for Android or Linux, the Native SDK limits the export of symbols to the `sentry_`-prefix. The option `SENTRY_EXPORT_SYMBOLS` is no longer available and the linker settings are constrained to the Native SDK and no longer `PUBLIC` to parent projects. ([#363](https://github.com/getsentry/sentry-native/pull/363))

**Features**:

- A session may be ended with a different status code. ([#801](https://github.com/getsentry/sentry-native/pull/801))

**Fixes**:

- Switch Crashpad transport on Linux to use libcurl ([#803](https://github.com/getsentry/sentry-native/pull/803), [crashpad#75](https://github.com/getsentry/crashpad/pull/75), [crashpad#79](https://github.com/getsentry/crashpad/pull/79))
- Avoid accidentally mutating CONTEXT when client-side stack walking in Crashpad ([#803](https://github.com/getsentry/sentry-native/pull/803), [crashpad#77](https://github.com/getsentry/crashpad/pull/77))
- Fix various mingw compilation issues ([#794](https://github.com/getsentry/sentry-native/pull/794), [crashpad#78](https://github.com/getsentry/crashpad/pull/78))

**Internal**:

- Updated Crashpad backend to 2023-02-07. ([#803](https://github.com/getsentry/sentry-native/pull/803), [crashpad#80](https://github.com/getsentry/crashpad/pull/80))
- CI: Updated GitHub Actions to test on LLVM-mingw. ([#797](https://github.com/getsentry/sentry-native/pull/797))
- Updated Breakpad backend to 2023-02-08. ([#805](https://github.com/getsentry/sentry-native/pull/805), [breakpad#34](https://github.com/getsentry/breakpad/pull/34))
- Updated libunwindstack to 2023-02-09. ([#807](https://github.com/getsentry/sentry-native/pull/807), [libunwindstack-ndk#7](https://github.com/getsentry/libunwindstack-ndk/pull/7))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@BogdanLivadariu](https://github.com/BogdanLivadariu)
- [@ShawnCZek](https://github.com/ShawnCZek)
- [@past-due](https://github.com/past-due)

## 0.5.4

**Fixes**:

- Better error messages in `sentry_transport_curl`. ([#777](https://github.com/getsentry/sentry-native/pull/777))
- Increased curl headers buffer size to 512 (in `sentry_transport_curl`). ([#784](https://github.com/getsentry/sentry-native/pull/784))
- Fix sporadic crash on Windows due to race condition when initializing background-worker thread-id. ([#785](https://github.com/getsentry/sentry-native/pull/785))
- Open the database file-lock on "UNIX" with `O_RDRW` ([#791](https://github.com/getsentry/sentry-native/pull/791))

**Internal**:

- Updated Breakpad and Crashpad backends to 2022-12-12. ([#778](https://github.com/getsentry/sentry-native/pull/778))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@cnicolaescu](https://github.com/cnicolaescu)

## 0.5.3

**Fixes**:

- Linux module-finder now also searches for code-id in ".note" ELF sections ([#775](https://github.com/getsentry/sentry-native/pull/775))

**Internal**:

- CI: updated github actions to upgrade deprecated node runners. ([#767](https://github.com/getsentry/sentry-native/pull/767))
- CI: upgraded Ubuntu to 20.04 for "old gcc" (v7) job due to deprecation. ([#768](https://github.com/getsentry/sentry-native/pull/768))

## 0.5.2

**Fixes**:

- Fix build when CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION is undefined. ([crashpad#73](https://github.com/getsentry/crashpad/pull/73))

**Internal**:

- Updated Breakpad and Crashpad backends to 2022-10-17. ([#765](https://github.com/getsentry/sentry-native/pull/765))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@AenBleidd](https://github.com/AenBleidd)

## 0.5.1

**Features**:

- Crashpad on Windows now supports `fast-fail` crashes via a registered Windows Error Reporting (WER) module. ([#735](https://github.com/getsentry/sentry-native/pull/735))

**Fixes**:

- Fix "flush" implementation of winhttp transport. ([#763](https://github.com/getsentry/sentry-native/pull/763))

**Internal**:

- Updated libunwindstack-ndk submodule to 2022-09-16. ([#759](https://github.com/getsentry/sentry-native/pull/759))
- Updated Breakpad and Crashpad backends to 2022-09-14. ([#735](https://github.com/getsentry/sentry-native/pull/735))
- Be more defensive around transactions ([#757](https://github.com/getsentry/sentry-native/pull/757))
- Added a CI timeout for the Android simulator start. ([#764](https://github.com/getsentry/sentry-native/pull/764))

## 0.5.0

**Features**:

- Provide `on_crash()` callback to allow clients to act on detected crashes.
  Users often inquired about distinguishing between crashes and "normal" events in the `before_send()` hook. `on_crash()` can be considered a replacement for `before_send()` for crash events, where the goal is to use `before_send()` only for normal events, while `on_crash()` is only invoked for crashes. This change is backward compatible for current users of `before_send()` and allows gradual migration to `on_crash()` ([see the docs for details](https://docs.sentry.io/platforms/native/configuration/filtering/)). ([#724](https://github.com/getsentry/sentry-native/pull/724), [#734](https://github.com/getsentry/sentry-native/pull/734))

**Fixes**:

- Make Windows ModuleFinder more resilient to missing Debug Info ([#732](https://github.com/getsentry/sentry-native/pull/732))
- Aligned pre-send event processing in `sentry_capture_event()` with the [cross-SDK session filter order](https://develop.sentry.dev/sdk/sessions/#filter-order) ([#729](https://github.com/getsentry/sentry-native/pull/729))
- Align the default value initialization for the `environment` payload attribute with the [developer documentation](https://develop.sentry.dev/sdk/event-payloads/#optional-attribute) ([#739](https://github.com/getsentry/sentry-native/pull/739))
- Iterate all debug directory entries when parsing PE modules for a valid CodeView record ([#740](https://github.com/getsentry/sentry-native/pull/740))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@espkk](https://github.com/espkk)

## 0.4.18

**Features**:

- The crashpad backend now captures thread names. ([#725](https://github.com/getsentry/sentry-native/pull/725))
- The inproc backend now captures the context registers. ([#714](https://github.com/getsentry/sentry-native/pull/714))
- A new set of APIs to get the sentry SDK version at runtime. ([#726](https://github.com/getsentry/sentry-native/pull/726))
- Add more convenient APIs to attach stack traces to exception or thread values. ([#723](https://github.com/getsentry/sentry-native/pull/723))
- Allow disabling the crash reporting backend at runtime. ([#717](https://github.com/getsentry/sentry-native/pull/717))

**Fixes**:

- Improved heuristics flagging sessions as "crashed". ([#719](https://github.com/getsentry/sentry-native/pull/719))

**Internal**:

- Updated Breakpad and Crashpad backends to 2022-06-14. ([#725](https://github.com/getsentry/sentry-native/pull/725))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@olback](https://github.com/olback)

## 0.4.17

**Fixes**:

- sentry-native now successfully builds when examples aren't included. ([#702](https://github.com/getsentry/sentry-native/pull/702))

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@AenBleidd](https://github.com/AenBleidd)

## 0.4.16

**Features**:

- Removed the `SENTRY_PERFORMANCE_MONITORING` compile flag requirement to access performance monitoring in the Sentry SDK. Performance monitoring is now available to everybody who has opted into the experimental API.
- New API to check whether the application has crashed in the previous run: `sentry_get_crashed_last_run()` and `sentry_clear_crashed_last_run()` ([#685](https://github.com/getsentry/sentry-native/pull/685)).
- Allow overriding the SDK name at build time - set the `SENTRY_SDK_NAME` CMake cache variable.
- More aggressively prune the Crashpad database. ([#698](https://github.com/getsentry/sentry-native/pull/698))

**Internal**:

- Project IDs are now treated as opaque strings instead of integer values. ([#690](https://github.com/getsentry/sentry-native/pull/690))
- Updated Breakpad and Crashpad backends to 2022-04-12. ([#696](https://github.com/getsentry/sentry-native/pull/696))

**Fixes**:

- Updated CI as well as list of supported platforms to reflect Windows Server 2016, and therefore MSVC 2017 losing active support.
- Correctly free Windows Mutexes in Crashpad backend.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@zhaowq32](https://github.com/zhaowq32)

## 0.4.15

**Fixes**:

- Fix contexts from the scope not being attached to events correctly.
- Improve performance of event serialization.

## 0.4.14

**Features**:

- The Sentry SDK now has experimental support for performance monitoring.
  The performance monitoring API allows manually creating transactions and instrumenting spans, and offers APIs for distributed tracing.
  The API is currently disabled by default and needs to be enabled via a compile-time `SENTRY_PERFORMANCE_MONITORING` flag.
  For more information, take a look at the more detailed [documentation of performance monitoring](https://docs.sentry.io/platforms/native/performance/).
- Sentry now has an explicit `sentry_flush` method that blocks the calling thread for the given time, waiting for the transport queue to be flushed. Custom transports need to implement a new `flush_hook` for this to work.

**Fixes**:

- Fix Sentry API deadlocking when the SDK was not initialized (or `sentry_init` failed).
- The rate limit handling of the default transports was updated to match the expected behavior.
- The Windows OS version is now read from the Registry and is more accurate.
- The `SENTRY_LIBRARY_TYPE` CMake option is now correctly honored.
- The Linux Modulefinder was once again improved to increase its memory safety and reliability.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@Mixaill](https://github.com/Mixaill)

## 0.4.13

**Features**:

- Add client-side stackwalking on Linux, Windows, and macOS (disabled by default).
- CMake: add ability to set solution folder name.
- Add AIX support.

**Fixes**:

- CMake: check whether libcurl was already found.
- Increment CXX standard version to 14 to allow crashpad to build.

**Internal**:

- Update Crashpad and Breakpad submodules to 2021-12-03.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@Mixaill](https://github.com/Mixaill)
- [@ladislavmacoun](https://github.com/ladislavmacoun)
- [@NattyNarwhal](https://github.com/NattyNarwhal)
- [@mjvankampen](https://github.com/mjvankampen)

## 0.4.12

**Features**:

- Make the shutdown timeout configurable via `sentry_options_set_shutdown_timeout`.

**Fixes**:

- The crashpad backend compiles with mingw again.
- Build System improvements.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@irov](https://github.com/irov)
- [@past-due](https://github.com/past-due)
- [@andrei-mu](https://github.com/andrei-mu)
- [@rpadaki](https://github.com/rpadaki)

## 0.4.11

**Fixes**:

- The crashpad backend now respects the `max_breadcrumbs` setting.
- Hanging HTTP requests will now be canceled on shutdown in the WinHTTP transport.
- The Modulefinder and Android unwinder now use safer memory access.
- Possible races and deadlocks have been fixed in `init`/`close`, and in API related to sessions.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@smibe](https://github.com/smibe)

## 0.4.10

**Fixes**:

- Fix a potential deadlock in macOS modulefinder.
- Lower Stack usage, to lower change of stack overflows.
- Avoid a double-free when parsing an invalid DSN.
- Improvements to Unity Builds and 32-bit Builds.
- Fix infinite recursion in signal handler by correctly cleaning up on shutdown.

**Internal**:

- Update Crashpad and Breakpad submodules to 2021-06-14.

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@janisozaur](https://github.com/janisozaur)
- [@bschatt](https://github.com/bschatt)
- [@GenuineAster](https://github.com/GenuineAster)

## 0.4.9

**Features**:

- Rewrote the Linux modulefinder which should now work correctly when encountering gaps in the memory mapping of loaded libraries, and supports libraries loaded from a file offset, such as libraries loaded directly from `.apk` files on Android.
- Invoke the `before_send` hook at time of a hard crash when using the Windows or Linux Crashpad backend.
- Added the following new convenience functions:
  - `sentry_value_new_exception`
  - `sentry_value_new_thread`
  - `sentry_value_new_stacktrace`
  - `sentry_event_add_exception`
  - `sentry_event_add_thread`
  - The `sentry_event_value_add_stacktrace` is deprecated.
- Renamed `sentry_shutdown` to `sentry_close`, though the old function is still available.
- Updated Qt integration to Qt 6.

**Fixes**:

- Optimized and fixed bugs in the JSON parser/serializer.
- Build fixes for PPC and universal macOS.
- Fixes to build using musl libc.
- Correctness fixes around printf and strftime usage.
- Allow building and running on older macOS versions.

**Internal**:

- Update Crashpad and Breakpad submodules to 2021-04-12

**Thank you**:

Features, fixes and improvements in this release have been contributed by:

- [@mastertheknife](https://github.com/mastertheknife)
- [@torarnv](https://github.com/torarnv)
- [@encounter](https://github.com/encounter)

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
