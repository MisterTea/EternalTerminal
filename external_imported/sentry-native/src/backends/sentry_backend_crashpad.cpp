extern "C" {
#include "sentry_boot.h"

#include "sentry_alloc.h"
#include "sentry_attachment.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_cpu_relax.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_logger.h"
#include "sentry_logs.h"
#include "sentry_metrics.h"
#include "sentry_options.h"
#ifdef SENTRY_PLATFORM_WINDOWS
#    include "sentry_os.h"
#endif
#include "sentry_path.h"
#include "sentry_screenshot.h"
#include "sentry_session_replay.h"
#include "sentry_sync.h"
#include "sentry_transport.h"
#include "sentry_value.h"
#ifdef SENTRY_PLATFORM_LINUX
#    include "sentry_unix_pageallocator.h"
#endif
#include "sentry_utils.h"
#include "sentry_uuid.h"
#include "transports/sentry_disk_transport.h"
}

#include <map>
#include <vector>

#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wunused-parameter"
#    pragma GCC diagnostic ignored "-Wmultichar"
#elif defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4100) // unreferenced formal parameter
#endif

#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wundef"
#    pragma clang diagnostic ignored "-Wsign-conversion"
#    pragma clang diagnostic ignored "-Wextra-semi-stmt"
#    pragma clang diagnostic ignored "-Wdocumentation-unknown-command"
#    pragma clang diagnostic ignored "-Wdocumentation"
#    pragma clang diagnostic ignored "-Wsuggest-destructor-override"
#    pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#    pragma clang diagnostic ignored "-Wlanguage-extension-token"
#    pragma clang diagnostic ignored "-Wfour-char-constants"
#    pragma clang diagnostic ignored                                           \
        "-Winconsistent-missing-destructor-override"
#endif
#include "base/logging.h"
#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/prune_crash_reports.h"
#include "client/settings.h"
#ifdef __clang__
#    pragma clang diagnostic pop
#endif
#if defined(_WIN32)
#    include "util/win/termination_codes.h"
#endif

#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#    pragma warning(pop)
#endif

// Provide an accessor for path here, since crashpad uses platform-dependent
// strings in the same interface and thus the same code could require access
// to both encodings across platforms. This is usually not the case, as path_w
// is used in code sections or translation units solely building for Windows.
#ifdef SENTRY_PLATFORM_WINDOWS
#    define SENTRY_PATH_PLATFORM_STR(PATH) PATH->path_w
#else
#    define SENTRY_PATH_PLATFORM_STR(PATH) PATH->path
#endif

template <typename T>
static void
safe_delete(T *&ptr)
{
    delete ptr;
    ptr = nullptr;
}

extern "C" {

#ifdef SENTRY_PLATFORM_LINUX
#    include <unistd.h>
#    define SIGNAL_STACK_SIZE 65536
static stack_t g_signal_stack;

#    include "util/posix/signals.h"

// This list was taken from crashpad's util/posix/signals.cc file
// and is used to know which signals we need to reset to default
// when shutting down the backend
constexpr int g_CrashSignals[] = {
    SIGABRT,
    SIGBUS,
    SIGFPE,
    SIGILL,
    SIGQUIT,
    SIGSEGV,
    SIGSYS,
    SIGTRAP,
#    if defined(SIGEMT)
    SIGEMT,
#    endif // defined(SIGEMT)
    SIGXCPU,
    SIGXFSZ,
};
#endif

static_assert(std::atomic<bool>::is_always_lock_free,
    "The crashpad-backend requires lock-free atomic<bool> to safely handle "
    "crashes");

typedef struct {
    crashpad::CrashReportDatabase *db;
    crashpad::CrashpadClient *client;
    sentry_path_t *event_path;
    sentry_path_t *breadcrumb1_path;
    sentry_path_t *breadcrumb2_path;
    sentry_path_t *external_report_path;
    size_t num_breadcrumbs;
    std::atomic<bool> crashed;
    std::atomic<bool> scope_flush;
    sentry_uuid_t crash_event_id;
} crashpad_state_t;

/**
 * Correctly destruct C++ members of the crashpad state.
 */
static void
crashpad_state_dtor(crashpad_state_t *state)
{
    safe_delete(state->client);
    safe_delete(state->db);
}

static void
crashpad_backend_user_consent_changed(sentry_backend_t *backend)
{
    auto *data = static_cast<crashpad_state_t *>(backend->data);
    if (!data->db || !data->db->GetSettings()) {
        return;
    }
    const bool enabled = !sentry__should_skip_upload();
    bool paused = false;
    if (!enabled) {
        SENTRY_WITH_OPTIONS (options) {
            paused = options->cache_keep || options->http_retry;
        }
    }
    auto *settings = data->db->GetSettings();
    settings->SetUploadsEnabled(enabled);
    settings->SetUploadsPaused(paused);
    if (enabled && data->client) {
        data->client->RequestRetry();
    }
}

#ifdef SENTRY_PLATFORM_WINDOWS
static void
crashpad_register_wer_module(
    const sentry_path_t *absolute_handler_path, const crashpad_state_t *data)
{
    windows_version_t win_ver;
    if (!sentry__get_windows_version(&win_ver) || win_ver.build < 19041) {
        SENTRY_WARN("Crashpad WER module not registered, because Windows "
                    "doesn't meet version requirements (build >= 19041).");
        return;
    }
    sentry_path_t *handler_dir = sentry__path_dir(absolute_handler_path);
    sentry_path_t *wer_path = nullptr;
    if (handler_dir) {
        wer_path = sentry__path_join_str(handler_dir, "crashpad_wer.dll");
        sentry__path_free(handler_dir);
    }

    if (wer_path && sentry__path_is_file(wer_path)) {
        SENTRY_DEBUGF(
            "registering crashpad WER handler \"%s\"", wer_path->path);

        // The WER handler needs to be registered in the registry first.
        constexpr DWORD dwOne = 1;
        const LSTATUS reg_res = RegSetKeyValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\Windows Error Reporting\\"
            L"RuntimeExceptionHelperModules",
            wer_path->path_w, REG_DWORD, &dwOne, sizeof(DWORD));
        if (reg_res != ERROR_SUCCESS) {
            SENTRY_WARN("registering crashpad WER handler in registry failed");
        } else {
            const std::wstring wer_path_string(wer_path->path_w);
            if (!data->client->RegisterWerModule(wer_path_string)) {
                SENTRY_WARN("registering crashpad WER handler module failed");
            }
        }

        sentry__path_free(wer_path);
    } else {
        SENTRY_WARN("crashpad WER handler module not found");
    }
}
#endif

static void
flush_scope_to_event(const sentry_path_t *event_path,
    const sentry_options_t *options, sentry_value_t crash_event)
{
    SENTRY_WITH_SCOPE (scope) {
        // we want the scope without any modules or breadcrumbs
        sentry__scope_apply_to_event(
            scope, options, crash_event, SENTRY_SCOPE_NONE);
    }

    size_t mpack_size;
    char *mpack = sentry_value_to_msgpack(crash_event, &mpack_size);
    sentry_value_decref(crash_event);
    if (!mpack) {
        return;
    }

    int rv = sentry__path_write_buffer(event_path, mpack, mpack_size);
    sentry_free(mpack);

    if (rv != 0) {
        SENTRY_WARN("flushing scope to msgpack failed");
    }
}

// Prepares an envelope with DSN, event ID, and session if available, for an
// external crash reporter.
static void
flush_external_crash_report(
    const sentry_options_t *options, const sentry_uuid_t *crash_event_id)
{
    sentry_envelope_t *envelope = sentry__envelope_new();
    if (!envelope) {
        return;
    }
    sentry__envelope_set_event_id(envelope, crash_event_id);
    if (options->session) {
        sentry__envelope_add_session(envelope, options->session);
    }

    if (options->cache_keep) {
        sentry__envelope_set_header(envelope, "cache_dir",
            sentry_value_new_string(options->run->cache_path->path));
    }
    sentry__run_write_external(options->run, envelope);
    sentry_envelope_free(envelope);
}

// This function is necessary for macOS since it has no `FirstChanceHandler`.
// but it is also necessary on Windows if the WER handler is enabled.
// This means we have to continuously flush the scope on
// every change so that `__sentry_event` is ready to upload when the crash
// happens. With platforms that have a `FirstChanceHandler` we can do that
// once in the handler. No need to share event- or crashpad-state mutation.
static void
crashpad_backend_flush_scope(
    sentry_backend_t *backend, const sentry_options_t *options)
{
#if defined(SENTRY_PLATFORM_LINUX)
    (void)backend;
    (void)options;
#else
    auto *data = static_cast<crashpad_state_t *>(backend->data);
    bool expected = false;

    //
    if (!data->event_path || data->crashed.load(std::memory_order_relaxed)
        || !data->scope_flush.compare_exchange_strong(
            expected, true, std::memory_order_acquire)) {
        return;
    }

    sentry_value_t event = sentry_value_new_object();
    sentry_value_set_by_key(
        event, "event_id", sentry__value_new_uuid(&data->crash_event_id));
    // Since this will only be uploaded in case of a crash we must make this
    // event fatal.
    sentry_value_set_by_key(
        event, "level", sentry__value_new_level(SENTRY_LEVEL_FATAL));

    flush_scope_to_event(data->event_path, options, event);
    if (data->external_report_path) {
        flush_external_crash_report(options, &data->crash_event_id);
    }
    data->scope_flush.store(false, std::memory_order_release);
#endif
}

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_WINDOWS)
static void
flush_scope_from_handler(
    const sentry_options_t *options, sentry_value_t crash_event)
{
    auto state = static_cast<crashpad_state_t *>(options->backend->data);

    // this blocks any further calls to `crashpad_backend_flush_scope`
    state->crashed.store(true, std::memory_order_relaxed);

    // busy-wait until any in-progress scope flushes are finished
    bool expected = false;
    while (!state->scope_flush.compare_exchange_strong(
        expected, true, std::memory_order_acquire)) {
        expected = false;
        sentry__cpu_relax();
    }

    // now we are the sole flusher and can flush into the crash event
    flush_scope_to_event(state->event_path, options, crash_event);
    if (state->external_report_path) {
        flush_external_crash_report(options, &state->crash_event_id);
    }
}

#    ifdef SENTRY_PLATFORM_WINDOWS
static bool
crashpad_handler(EXCEPTION_POINTERS *ExceptionInfo)
{
#    else
static bool
crashpad_handler(int signum, siginfo_t *info, ucontext_t *user_context)
{
    sentry__page_allocator_enable();
    (void)!sentry__enter_signal_handler();
#    endif
    // Disable logging during crash handling if the option is set
    SENTRY_WITH_OPTIONS (options) {
        if (!options->enable_logging_when_crashed) {
            sentry__logger_disable();
        }
    }

    SENTRY_INFO("flushing session and queue before crashpad handler");

    bool should_dump = true;

    SENTRY_WITH_OPTIONS (options) {
        auto state = static_cast<crashpad_state_t *>(options->backend->data);
        sentry_value_t crash_event
            = sentry__value_new_event_with_id(&state->crash_event_id);
        sentry_value_set_by_key(
            crash_event, "level", sentry__value_new_level(SENTRY_LEVEL_FATAL));

        if (options->on_crash_func) {
            sentry_ucontext_t uctx;
#    ifdef SENTRY_PLATFORM_WINDOWS
            uctx.exception_ptrs = *ExceptionInfo;
#    else
            uctx.signum = signum;
            uctx.siginfo = info;
            uctx.user_context = user_context;
#    endif

            SENTRY_DEBUG("invoking `on_crash` hook");
            crash_event = options->on_crash_func(
                &uctx, crash_event, options->on_crash_data);
        } else if (options->before_send_func) {
            SENTRY_DEBUG("invoking `before_send` hook");
            crash_event = options->before_send_func(
                crash_event, nullptr, options->before_send_data);
        }

        // Flush logs and metrics in a crash-safe manner before crash handling
        if (options->enable_logs) {
            sentry__logs_flush_crash_safe();
        }
        if (options->enable_metrics) {
            sentry__metrics_flush_crash_safe();
        }

        should_dump = !sentry_value_is_null(crash_event);

        if (should_dump) {
            sentry_value_incref(crash_event);
            flush_scope_from_handler(options, crash_event);
            sentry__write_crash_marker(options);

            sentry__record_errors_on_current_session(1);
            sentry_session_t *session = sentry__end_current_session_with_status(
                SENTRY_SESSION_STATUS_CRASHED);
            if (session) {
                sentry_envelope_t *envelope = sentry__envelope_new();
                sentry__envelope_add_session(envelope, session);

                // capture the envelope with the disk transport
                sentry_transport_t *disk_transport
                    = sentry_new_disk_transport(options->run);
                sentry__capture_envelope(disk_transport, envelope, options);
                sentry__transport_dump_queue(disk_transport, options->run);
                sentry_transport_free(disk_transport);
            }

            if (sentry__session_replay_has_pending(options)) {
                sentry_transport_t *replay_transport
                    = sentry_new_disk_transport(options->run);
                if (replay_transport) {
                    sentry__session_replay_flush_pending(
                        options, replay_transport, crash_event);
                    sentry__transport_dump_queue(
                        replay_transport, options->run);
                    sentry_transport_free(replay_transport);
                }
            }
            sentry_value_decref(crash_event);
        } else {
            SENTRY_DEBUG("event was discarded");
        }
        sentry__transport_dump_queue(options->transport, options->run);
    }

    SENTRY_INFO("handing control over to crashpad");

    // If we __don't__ want a minidump produced by crashpad we need to either
    // exit or longjmp at this point. The crashpad client handler which calls
    // back here (SetFirstChanceExceptionHandler) does the same if the
    // application is not shutdown via the crashpad_handler process.
    //
    // If we would return `true` here without changing any of the global signal-
    // handling state or rectifying the cause of the signal, this would turn
    // into a signal-handler/exception-filter loop, because some
    // signals/exceptions (like SIGSEGV) are unrecoverable.
    //
    // Ideally the SetFirstChanceExceptionHandler would accept more than a
    // boolean to differentiate between:
    //
    // * we accept our fate and want a minidump (currently returning `false`)
    // * we accept our fate and don't want a minidump (currently not available)
    // * we rectified the situation, so crashpads signal-handler can simply
    //   return, thereby letting the not-rectified signal-cause trigger a
    //   signal-handler/exception-filter again, which probably leads to us
    //   (currently returning `true`)
    //
    // TODO(supervacuus):
    // * we need integration tests for more signal/exception types not only
    //   for unmapped memory access (which is the current crash in example.c).
    // * we should adapt the SetFirstChanceExceptionHandler interface in
    // crashpad
    if (!should_dump) {
#    ifdef SENTRY_PLATFORM_WINDOWS
        TerminateProcess(GetCurrentProcess(),
            crashpad::TerminationCodes::kTerminationCodeCrashNoDump);
#    else
        _exit(EXIT_FAILURE);
#    endif
    }

#    ifndef SENTRY_PLATFORM_WINDOWS
    sentry__leave_signal_handler();
#    endif

    // we did not "handle" the signal, so crashpad should do that.
    return false;
}
#endif

static sentry_value_t
read_msgpack_file(const sentry_path_t *path)
{
    size_t size;
    char *data = sentry__path_read_to_buffer(path, &size);
    if (!data) {
        return sentry_value_new_null();
    }
    sentry_value_t value = sentry__value_from_msgpack(data, size);
    sentry_free(data);
    return value;
}

static sentry_path_t *
report_attachments_dir(const crashpad::CrashReportDatabase::Report &report,
    const sentry_options_t *options)
{
    sentry_path_t *attachments_root
        = sentry__path_join_str(options->database_path, "attachments");
    if (!attachments_root) {
        return nullptr;
    }

    sentry_path_t *attachments_dir = sentry__path_join_str(
        attachments_root, report.uuid.ToString().c_str());

    sentry__path_free(attachments_root);
    return attachments_dir;
}

// Converts a completed crashpad report into a sentry envelope by reading the
// event, breadcrumbs, and attachments from the report's attachments directory.
static sentry_envelope_t *
report_to_envelope(const crashpad::CrashReportDatabase::Report &report,
    const sentry_options_t *options)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    sentry_path_t *minidump_path
        = sentry__path_from_wstr(report.file_path.value().c_str());
#else
    sentry_path_t *minidump_path
        = sentry__path_from_str(report.file_path.value().c_str());
#endif
    sentry_path_t *attachments_dir = report_attachments_dir(report, options);

    if (!minidump_path || !attachments_dir) {
        sentry__path_free(minidump_path);
        sentry__path_free(attachments_dir);
        return nullptr;
    }

    sentry_value_t event = sentry_value_new_null();
    sentry_value_t breadcrumbs1 = sentry_value_new_null();
    sentry_value_t breadcrumbs2 = sentry_value_new_null();
    sentry_attachment_t *attachments = nullptr;

    sentry_pathiter_t *iter = sentry__path_iter_directory(attachments_dir);
    if (iter) {
        const sentry_path_t *path;
        while ((path = sentry__pathiter_next(iter)) != nullptr) {
            const char *filename = sentry__path_filename(path);
            if (strcmp(filename, "__sentry-event") == 0) {
                event = read_msgpack_file(path);
            } else if (strcmp(filename, "__sentry-breadcrumb1") == 0) {
                breadcrumbs1 = read_msgpack_file(path);
            } else if (strcmp(filename, "__sentry-breadcrumb2") == 0) {
                breadcrumbs2 = read_msgpack_file(path);
            } else {
                sentry__attachments_add_path(
                    &attachments, sentry__path_clone(path), nullptr, nullptr);
            }
        }
        sentry__pathiter_free(iter);
    }
    sentry__path_free(attachments_dir);

    sentry_envelope_t *envelope = nullptr;
    if (!sentry_value_is_null(event)) {
        envelope = sentry__envelope_new();
        if (envelope && options->dsn && options->dsn->is_valid) {
            sentry__envelope_set_header(envelope, "dsn",
                sentry_value_new_string(sentry_options_get_dsn(options)));
        }
    }
    if (envelope) {
        sentry_value_set_by_key(event, "breadcrumbs",
            sentry__value_merge_breadcrumbs(
                breadcrumbs1, breadcrumbs2, options->max_breadcrumbs));
        sentry__attachments_add_path(&attachments, minidump_path,
            SENTRY_ATTACHMENT_TYPE_MINIDUMP, nullptr);

        if (sentry__envelope_add_event(envelope, event)) {
            sentry__envelope_add_attachments(envelope, attachments, options);
            if (options->run) {
                sentry__cache_attachment_refs(envelope, attachments, options,
                    options->run->cache_path, options->run->run_path);
            }
        } else {
            sentry_value_decref(event);
            sentry_envelope_free(envelope);
            envelope = nullptr;
        }
    } else {
        sentry__path_free(minidump_path);
        sentry_value_decref(event);
    }

    sentry_value_decref(breadcrumbs1);
    sentry_value_decref(breadcrumbs2);
    sentry__attachments_free(attachments);

    return envelope;
}

// Caches completed crashpad reports as sentry envelopes and removes them from
// the crashpad database. Called during startup before the handler is started.
static void
process_completed_reports(
    crashpad_state_t *state, const sentry_options_t *options)
{
    if (!state || !state->db || !options || !options->cache_keep) {
        return;
    }

    std::vector<crashpad::CrashReportDatabase::Report> reports;
    if (state->db->GetCompletedReports(&reports)
            != crashpad::CrashReportDatabase::kNoError
        || reports.empty()) {
        return;
    }

    SENTRY_DEBUGF("caching %zu completed reports", reports.size());

    sentry_path_t *cache_dir = options->run->cache_path;
    if (sentry__path_create_dir_all(cache_dir) != 0) {
        SENTRY_WARN("failed to create cache dir");
        return;
    }

    for (const auto &report : reports) {
        std::string filename = report.uuid.ToString() + ".envelope";
        sentry_envelope_t *envelope = report_to_envelope(report, options);
        if (!envelope) {
            SENTRY_WARNF("failed to convert \"%s\"", filename.c_str());
            continue;
        }
        if (sentry__envelope_write_to_cache(envelope, cache_dir) != 0) {
            SENTRY_WARNF("failed to cache \"%s\"", filename.c_str());
        } else if (state->db->DeleteReport(report.uuid)
            != crashpad::CrashReportDatabase::kNoError) {
            SENTRY_WARNF("failed to delete \"%s\"", filename.c_str());
        }
        sentry_envelope_free(envelope);
    }
}

static int
crashpad_backend_startup(
    sentry_backend_t *backend, const sentry_options_t *options)
{
    sentry_path_t *owned_handler_path = nullptr;
    sentry_path_t *handler_path = options->handler_path;
    if (!handler_path) {
        if (sentry_path_t *current_exe = sentry__path_current_exe()) {
            sentry_path_t *exe_dir = sentry__path_dir(current_exe);
            sentry__path_free(current_exe);
            if (exe_dir) {
                handler_path = sentry__path_join_str(exe_dir,
#ifdef SENTRY_PLATFORM_WINDOWS
                    "crashpad_handler.exe"
#else
                    "crashpad_handler"
#endif
                );
                owned_handler_path = handler_path;
                sentry__path_free(exe_dir);
            }
        }
    }

#if defined(SENTRY_PLATFORM_WINDOWS) && !defined(SENTRY_BUILD_SHARED)          \
    && defined(SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT)
    sentry__set_default_thread_stack_guarantee();
#endif

    // The crashpad client uses shell lookup rules (absolute path, relative
    // path, or bare executable name that is looked up in $PATH).
    // However, it crashes hard when it can't resolve the handler, so we make
    // sure to resolve and check for it first.
    sentry_path_t *absolute_handler_path = sentry__path_absolute(handler_path);
    sentry__path_free(owned_handler_path);
    if (!absolute_handler_path
        || !sentry__path_is_file(absolute_handler_path)) {
        SENTRY_WARN("unable to start crashpad backend, invalid handler_path");
        sentry__path_free(absolute_handler_path);
        return 1;
    }

    SENTRY_DEBUGF("starting crashpad backend with handler \"%s\"",
        absolute_handler_path->path);
    sentry_path_t *current_run_folder = options->run->run_path;
    auto *data = static_cast<crashpad_state_t *>(backend->data);

    // pre-generate event ID for a potential future crash to be able to
    // associate feedback with the crash event.
    data->crash_event_id = sentry__new_event_id();

    base::FilePath database(SENTRY_PATH_PLATFORM_STR(options->database_path));
    base::FilePath handler(SENTRY_PATH_PLATFORM_STR(absolute_handler_path));

    std::map<std::string, std::string> annotations;
    std::vector<base::FilePath> attachments;

    // register attachments
    for (sentry_attachment_t *attachment = options->attachments; attachment;
        attachment = attachment->next) {
        attachments.emplace_back(SENTRY_PATH_PLATFORM_STR(attachment->path));
    }

    // and add the serialized event, and two rotating breadcrumb files
    // as attachments and make sure the files exist
    data->event_path
        = sentry__path_join_str(current_run_folder, "__sentry-event");
    data->breadcrumb1_path
        = sentry__path_join_str(current_run_folder, "__sentry-breadcrumb1");
    data->breadcrumb2_path
        = sentry__path_join_str(current_run_folder, "__sentry-breadcrumb2");

    sentry__path_touch(data->event_path);
    sentry__path_touch(data->breadcrumb1_path);
    sentry__path_touch(data->breadcrumb2_path);

    attachments.insert(attachments.end(),
        { base::FilePath(SENTRY_PATH_PLATFORM_STR(data->event_path)),
            base::FilePath(SENTRY_PATH_PLATFORM_STR(data->breadcrumb1_path)),
            base::FilePath(SENTRY_PATH_PLATFORM_STR(data->breadcrumb2_path)) });

    base::FilePath screenshot;
    if (options->attach_screenshot) {
        sentry_path_t *screenshot_path = sentry__screenshot_get_path(options);
        screenshot = base::FilePath(SENTRY_PATH_PLATFORM_STR(screenshot_path));
        sentry__path_free(screenshot_path);
    }

    base::FilePath crash_reporter;
    base::FilePath crash_envelope;
    if (options->external_crash_reporter) {
        char *filename
            = sentry__uuid_as_filename(&data->crash_event_id, ".envelope");
        data->external_report_path
            = sentry__path_join_str(options->run->external_path, filename);
        sentry_free(filename);

        if (data->external_report_path) {
            crash_reporter = base::FilePath(
                SENTRY_PATH_PLATFORM_STR(options->external_crash_reporter));
            crash_envelope = base::FilePath(
                SENTRY_PATH_PLATFORM_STR(data->external_report_path));
        }
    }

    std::vector<std::string> arguments { "--no-rate-limit" };

    // Map sentry's log level to mini_chromium's LogSeverity. They diverge at
    // FATAL (sentry=3, mini_chromium=4); otherwise 1:1.
    int level = static_cast<int>(options->logger.logger_level);
    switch (options->logger.logger_level) {
    case SENTRY_LEVEL_TRACE:
    case SENTRY_LEVEL_DEBUG:
        level = -1; // LOG_VERBOSE
        break;
    case SENTRY_LEVEL_INFO:
    case SENTRY_LEVEL_WARNING:
    case SENTRY_LEVEL_ERROR:
        // LOG_INFO/WARNING/ERROR (0/1/2) match 1:1
        break;
    case SENTRY_LEVEL_FATAL:
        level = 4; // LOG_FATAL
        break;
    }

    sentry_path_t *log_path
        = sentry__path_join_str(current_run_folder, "crashpad-handler.log");
    if (log_path) {
        arguments.push_back(std::string("--log-file=") + log_path->path);
        sentry__path_free(log_path);
    }
    if (options->debug) {
        logging::LoggingSettings settings;
        settings.logging_dest = logging::LOG_TO_STDERR;
        settings.min_log_level = level;
        logging::InitLogging(settings);
        arguments.push_back("--log-level=" + std::to_string(level));
    }

    char report_id[37];
    sentry_uuid_as_string(&data->crash_event_id, report_id);

    // Initialize database first, flushing the consent later on as part of
    // `sentry_init` will persist the upload flag.
    data->db = crashpad::CrashReportDatabase::Initialize(database).release();
    process_completed_reports(data, options);
    data->client = new (std::nothrow) crashpad::CrashpadClient;
    char *minidump_url
        = sentry__dsn_get_minidump_url(options->dsn, options->user_agent);
    if (minidump_url) {
        SENTRY_DEBUGF("using minidump URL \"%s\"", minidump_url);
    }
    const char *env_proxy = options->dsn
        ? getenv(options->dsn->is_secure ? "https_proxy" : "http_proxy")
        : nullptr;
    const char *proxy_url = options->proxy ? options->proxy
        : env_proxy                        ? env_proxy
                                           : "";
#ifdef SENTRY_PLATFORM_LINUX
    // explicitly set an empty proxy to avoid reading from env. vars. on Linux
    if (options->proxy && strcmp(options->proxy, "") == 0) {
        proxy_url = "<empty>";
    }
#endif
    bool success = data->client->StartHandler(handler, database, database,
        minidump_url ? minidump_url : "", proxy_url, annotations, arguments,
        /* restartable */ true,
        /* asynchronous_start */ false, attachments, screenshot,
        options->crashpad_wait_for_upload, crash_reporter, crash_envelope,
        report_id);
    sentry_free(minidump_url);

#ifdef SENTRY_PLATFORM_WINDOWS
    crashpad_register_wer_module(absolute_handler_path, data);
#endif

    sentry__path_free(absolute_handler_path);

    if (success) {
        SENTRY_INFO("started crashpad client handler");
    } else {
        SENTRY_WARN("failed to start crashpad client handler");
        // not calling `shutdown`
        crashpad_state_dtor(data);
        return 1;
    }

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_WINDOWS)
    crashpad::CrashpadClient::SetFirstChanceExceptionHandler(&crashpad_handler);
#endif
#ifdef SENTRY_PLATFORM_LINUX
    // Crashpad was recently changed to register its own signal stack, which for
    // whatever reason is not compatible with our own handler. so we override
    // that stack yet again to be able to correctly flush things out.
    // https://github.com/getsentry/crashpad/commit/06a688ddc1bc8be6f410e69e4fb413fc19594d04
    g_signal_stack.ss_sp = sentry_malloc(SIGNAL_STACK_SIZE);
    if (g_signal_stack.ss_sp) {
        g_signal_stack.ss_size = SIGNAL_STACK_SIZE;
        g_signal_stack.ss_flags = 0;
        sigaltstack(&g_signal_stack, nullptr);
    }
#endif

    crashpad::CrashpadInfo *crashpad_info
        = crashpad::CrashpadInfo::GetCrashpadInfo();

    if (!options->system_crash_reporter_enabled) {
        // Disable the system crash reporter. Especially on macOS, it takes
        // substantial time *after* crashpad has done its job.
        crashpad_info->set_system_crash_reporter_forwarding(
            crashpad::TriState::kDisabled);
    }

    if (options->crashpad_limit_stack_capture_to_sp) {
        // Enable stack capture limit to work around Wine/Proton TEB issues
        crashpad_info->set_limit_stack_capture_to_sp(
            crashpad::TriState::kEnabled);
    }

    return 0;
}

static void
crashpad_backend_shutdown(sentry_backend_t *backend)
{
#ifdef SENTRY_PLATFORM_LINUX
    // restore signal handlers to their default state
    for (const auto signal : g_CrashSignals) {
        if (crashpad::Signals::IsCrashSignal(signal)) {
            crashpad::Signals::InstallDefaultHandler(signal);
        }
    }
#endif

    crashpad_state_dtor(static_cast<crashpad_state_t *>(backend->data));

#ifdef SENTRY_PLATFORM_LINUX
    g_signal_stack.ss_flags = SS_DISABLE;
    sigaltstack(&g_signal_stack, nullptr);
    sentry_free(g_signal_stack.ss_sp);
    g_signal_stack.ss_sp = nullptr;
#endif
}

static void
crashpad_backend_add_breadcrumb(sentry_backend_t *backend,
    sentry_value_t breadcrumb, const sentry_options_t *options)
{
    auto *data = static_cast<crashpad_state_t *>(backend->data);

    size_t max_breadcrumbs = options->max_breadcrumbs;
    if (!max_breadcrumbs) {
        return;
    }

    bool first_breadcrumb = data->num_breadcrumbs % max_breadcrumbs == 0;

    const sentry_path_t *breadcrumb_file
        = data->num_breadcrumbs % (max_breadcrumbs * 2) < max_breadcrumbs
        ? data->breadcrumb1_path
        : data->breadcrumb2_path;
    data->num_breadcrumbs++;
    if (!breadcrumb_file) {
        return;
    }

    size_t mpack_size;
    char *mpack = sentry_value_to_msgpack(breadcrumb, &mpack_size);
    if (!mpack) {
        return;
    }

    int rv = first_breadcrumb
        ? sentry__path_write_buffer(breadcrumb_file, mpack, mpack_size)
        : sentry__path_append_buffer(breadcrumb_file, mpack, mpack_size);
    sentry_free(mpack);

    if (rv != 0) {
        SENTRY_WARN("flushing breadcrumb to msgpack failed");
    }
}

static void
crashpad_backend_free(sentry_backend_t *backend)
{
    auto *data = static_cast<crashpad_state_t *>(backend->data);
    sentry__path_free(data->event_path);
    sentry__path_free(data->breadcrumb1_path);
    sentry__path_free(data->breadcrumb2_path);
    sentry__path_free(data->external_report_path);
    delete data;
}

static void
crashpad_backend_except(
    sentry_backend_t *UNUSED(backend), const sentry_ucontext_t *context)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    crashpad::CrashpadClient::DumpAndCrash(
        const_cast<EXCEPTION_POINTERS *>(&context->exception_ptrs));
#else
    // TODO: Crashpad has the ability to do this on linux / mac but the
    // method interface is not exposed for it, a patch would be required
    (void)context;
#endif
}

static void
report_crash_time(
    uint64_t *crash_time, const crashpad::CrashReportDatabase::Report &report)
{
    // we do a `+ 1` here, because crashpad timestamps are second resolution,
    // but our sessions are ms resolution. at least in our integration tests, we
    // can have a session that starts at, e.g. `0.471`, whereas the crashpad
    // report will be `0`, which would mean our heuristic does not trigger due
    // to rounding.
    uint64_t time = (static_cast<uint64_t>(report.creation_time) + 1) * 1000000;
    if (time > *crash_time) {
        *crash_time = time;
    }
}

static uint64_t
crashpad_backend_last_crash(sentry_backend_t *backend)
{
    auto *data = static_cast<crashpad_state_t *>(backend->data);

    uint64_t crash_time = 0;

    std::vector<crashpad::CrashReportDatabase::Report> reports;
    if (data->db->GetCompletedReports(&reports)
        == crashpad::CrashReportDatabase::kNoError) {
        for (const crashpad::CrashReportDatabase::Report &report : reports) {
            report_crash_time(&crash_time, report);
        }
    }

    return crash_time;
}

class CachePruneCondition final : public crashpad::PruneCondition {
public:
    CachePruneCondition(size_t max_items, size_t max_size, time_t max_age)
        : max_items_(max_items)
        , item_count_(0)
        , max_size_(max_size)
        , measured_size_(0)
        , max_age_(max_age)
        , oldest_report_time_(time(nullptr) - max_age)
    {
    }

    bool
    ShouldPruneReport(
        const crashpad::CrashReportDatabase::Report &report) override
    {
        ++item_count_;
        measured_size_ += static_cast<size_t>(report.total_size);

        bool by_items = max_items_ > 0 && item_count_ > max_items_;
        bool by_size = max_size_ > 0 && measured_size_ > max_size_;
        bool by_age
            = max_age_ > 0 && report.creation_time < oldest_report_time_;
        return by_items || by_size || by_age;
    }

private:
    const size_t max_items_;
    size_t item_count_;
    const size_t max_size_;
    size_t measured_size_;
    const time_t max_age_;
    const time_t oldest_report_time_;
};

static void
crashpad_backend_prune_database(sentry_backend_t *backend)
{
    auto *data = static_cast<crashpad_state_t *>(backend->data);

    // For backwards compatibility, default to the parameters that were used
    // before the offline caching API was introduced. We wanted to eagerly
    // clean up reports older than 2 days, and limit the complete database
    // to a maximum of 8M. That might still have been a lot for an embedded
    // use-case, but minidumps on desktop can sometimes be quite large.
    time_t max_age = 2 * 24 * 60 * 60; // 2 days
    size_t max_size = 8 * 1024 * 1024; // 8 MB
    size_t max_items = 0;

    // When offline caching is enabled, the user has full control over these
    // parameters via the cache_max_* options.
    SENTRY_WITH_OPTIONS (options) {
        if (options->cache_keep) {
            max_age = options->cache_max_age;
            max_size = options->cache_max_size;
            max_items = options->cache_max_items;
        }
    }

    if (max_age > 0) {
        data->db->CleanDatabase(max_age);
    }

    CachePruneCondition condition(max_items, max_size, max_age);
    crashpad::PruneCrashReportDatabase(data->db, &condition);
}

#if defined(SENTRY_PLATFORM_WINDOWS) || defined(SENTRY_PLATFORM_LINUX)         \
    || defined(SENTRY_PLATFORM_MACOS)
static bool
ensure_unique_path(sentry_attachment_t *attachment)
{
    sentry_path_t *path = nullptr;
    SENTRY_WITH_OPTIONS (options) {
        path = sentry__path_unique(options->run->run_path,
            sentry__path_filename(attachment->filename));
    }
    if (!path) {
        return false;
    }

    sentry__path_free(attachment->path);
    attachment->path = path;
    return true;
}

static void
crashpad_backend_add_attachment(
    sentry_backend_t *backend, sentry_attachment_t *attachment)
{
    auto *data = static_cast<crashpad_state_t *>(backend->data);
    if (!data || !data->client) {
        return;
    }

    if (attachment->buf) {
        if (!ensure_unique_path(attachment)
            || sentry__path_write_buffer(
                   attachment->path, attachment->buf, attachment->buf_len)
                != 0) {
            SENTRY_WARNF("failed to write crashpad attachment \"%s\"",
                attachment->path->path);
        }
    }

    data->client->AddAttachment(
        base::FilePath(SENTRY_PATH_PLATFORM_STR(attachment->path)));
}

static void
crashpad_backend_remove_attachment(
    sentry_backend_t *backend, sentry_attachment_t *attachment)
{
    auto *data = static_cast<crashpad_state_t *>(backend->data);
    if (!data || !data->client) {
        return;
    }
    data->client->RemoveAttachment(
        base::FilePath(SENTRY_PATH_PLATFORM_STR(attachment->path)));

    if (attachment->buf && sentry__path_remove(attachment->path) != 0) {
        SENTRY_WARNF("failed to remove crashpad attachment \"%s\"",
            attachment->path->path);
    }
}
#endif

void
sentry__backend_preload(void)
{
}

sentry_backend_t *
sentry__backend_new(void)
{
    auto *backend = SENTRY_MAKE(sentry_backend_t);
    if (!backend) {
        return nullptr;
    }

    auto *data = new (std::nothrow) crashpad_state_t {};
    if (!data) {
        sentry_free(backend);
        return nullptr;
    }
    data->scope_flush = false;
    data->crashed = false;

    backend->startup_func = crashpad_backend_startup;
    backend->shutdown_func = crashpad_backend_shutdown;
    backend->except_func = crashpad_backend_except;
    backend->free_func = crashpad_backend_free;
    backend->flush_scope_func = crashpad_backend_flush_scope;
    backend->add_breadcrumb_func = crashpad_backend_add_breadcrumb;
    backend->user_consent_changed_func = crashpad_backend_user_consent_changed;
    backend->get_last_crash_func = crashpad_backend_last_crash;
    backend->prune_database_func = crashpad_backend_prune_database;
#if defined(SENTRY_PLATFORM_WINDOWS) || defined(SENTRY_PLATFORM_LINUX)         \
    || defined(SENTRY_PLATFORM_MACOS)
    backend->add_attachment_func = crashpad_backend_add_attachment;
    backend->remove_attachment_func = crashpad_backend_remove_attachment;
#endif
    backend->data = data;
    backend->can_capture_after_shutdown = true;

    return backend;
}
}
