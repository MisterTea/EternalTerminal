extern "C" {
#include "sentry_boot.h"

#include "sentry_alloc.h"
#include "sentry_attachment.h"
#include "sentry_backend.h"
#include "sentry_client_report.h"
#include "sentry_core.h"
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
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_tracing.h"
#include "sentry_transport.h"
#include "sentry_unix_pageallocator.h"
#include "transports/sentry_disk_transport.h"
}

#ifdef __GNUC__
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpedantic"
#    pragma GCC diagnostic ignored "-Wvariadic-macros"
#endif

#ifdef SENTRY_PLATFORM_WINDOWS
#    ifdef __clang__
#        pragma clang diagnostic push
#        pragma clang diagnostic ignored "-Wnested-anon-types"
#        pragma clang diagnostic ignored "-Wmicrosoft-enum-value"
#        pragma clang diagnostic ignored "-Wzero-length-array"
#    endif
#    include "client/windows/handler/exception_handler.h"
#    ifdef __clang__
#        pragma clang diagnostic pop
#    endif
#elif defined(SENTRY_PLATFORM_MACOS)
#    include "client/mac/handler/exception_handler.h"
#    include <sys/sysctl.h>
#    include <unistd.h>
#elif defined(SENTRY_PLATFORM_IOS)
#    include "client/ios/exception_handler_no_mach.h"
#else
#    include "client/linux/handler/exception_handler.h"
#endif

#ifdef __GNUC__
#    pragma GCC diagnostic pop
#endif

#ifdef SENTRY_PLATFORM_WINDOWS
static void
handle_sigabrt(EXCEPTION_POINTERS *exception_pointers)
{
    sentry_ucontext_t uctx = {};
    uctx.exception_ptrs = *exception_pointers;
    sentry_handle_exception(&uctx);
}

static bool
breakpad_backend_callback(const wchar_t *breakpad_dump_path,
    const wchar_t *minidump_id, void *UNUSED(context),
    EXCEPTION_POINTERS *exinfo, MDRawAssertionInfo *UNUSED(assertion),
    bool succeeded)
#elif defined(SENTRY_PLATFORM_DARWIN)
#    if defined(SENTRY_BREAKPAD_SYSTEM) || defined(SENTRY_PLATFORM_IOS)
static bool
breakpad_backend_callback(const char *breakpad_dump_path,
    const char *minidump_id, void *UNUSED(context), bool succeeded)
#    else
static bool
breakpad_backend_callback(const char *breakpad_dump_path,
    const char *minidump_id, void *UNUSED(context),
    breakpad_ucontext_t *user_context, bool succeeded)
#    endif
#else
static bool
breakpad_backend_callback(const google_breakpad::MinidumpDescriptor &descriptor,
    void *UNUSED(context), bool succeeded)
#endif
{
    // Disable logging during crash handling if the option is set
    SENTRY_WITH_OPTIONS (options) {
        if (!options->enable_logging_when_crashed) {
            sentry__logger_disable();
        }
    }

    SENTRY_SIGNAL_SAFE_LOG("INFO entering breakpad minidump callback");

    // this is a bit strange, according to docs, `succeeded` should be true when
    // a minidump file was successfully generated. however, when running our
    // integration tests on linux, we do receive `false` here even though the
    // minidump file exists and has a valid minidump magic. in either case, we
    // are in a crashing state, so we should capture a crash no matter what.
    // See:
    // https://github.com/google/breakpad/blob/428a01e8dea2555e037570a0b854137029a78cbf/src/client/linux/handler/exception_handler.h#L90-L102
    // https://github.com/google/breakpad/blob/428a01e8dea2555e037570a0b854137029a78cbf/src/client/linux/handler/exception_handler.cc#L564-L567
    // if (!succeeded) {
    //     SENTRY_WARN("breakpad failed creating minidump");
    //     return succeeded;
    // }

#ifndef SENTRY_PLATFORM_WINDOWS
    sentry__page_allocator_enable();
    (void)!sentry__enter_signal_handler();
#endif

    sentry_path_t *dump_path = nullptr;
#ifdef SENTRY_PLATFORM_WINDOWS
    sentry_path_t *tmp_path = sentry__path_from_wstr(breakpad_dump_path);
    dump_path = sentry__path_join_wstr(tmp_path, minidump_id);
    sentry__path_free(tmp_path);
    tmp_path = dump_path;
    dump_path = sentry__path_append_str(tmp_path, ".dmp");
    sentry__path_free(tmp_path);
#elif defined(SENTRY_PLATFORM_DARWIN)
    sentry_path_t *tmp_path = sentry__path_new(breakpad_dump_path);
    dump_path = sentry__path_join_str(tmp_path, minidump_id);
    sentry__path_free(tmp_path);
    tmp_path = dump_path;
    dump_path = sentry__path_append_str(tmp_path, ".dmp");
    sentry__path_free(tmp_path);
#else
    dump_path = sentry__path_new(descriptor.path());
#endif
    sentry_value_t event = sentry_value_new_event();
    sentry_value_set_by_key(
        event, "level", sentry__value_new_level(SENTRY_LEVEL_FATAL));

    SENTRY_WITH_OPTIONS (options) {
        sentry__write_crash_marker(options);

        sentry_value_t transaction
            = sentry__trace_finish(SENTRY_SPAN_STATUS_ABORTED);

        bool should_handle = true;

        if (options->on_crash_func) {
            sentry_ucontext_t *uctx = nullptr;

#if defined(SENTRY_PLATFORM_DARWIN)                                            \
    && !(defined(SENTRY_BREAKPAD_SYSTEM) || defined(SENTRY_PLATFORM_IOS))
            sentry_ucontext_t uctx_data;
            uctx_data.user_context = user_context;
            uctx = &uctx_data;
#endif

#ifdef SENTRY_PLATFORM_WINDOWS
            sentry_ucontext_t uctx_data;
            uctx_data.exception_ptrs = *exinfo;
            uctx = &uctx_data;
#endif

            SENTRY_SIGNAL_SAFE_LOG("DEBUG invoking `on_crash` hook");
            sentry_value_t result
                = options->on_crash_func(uctx, event, options->on_crash_data);
            should_handle = !sentry_value_is_null(result);
        }

        // Flush logs and metrics in a crash-safe manner before crash handling
        if (options->enable_logs) {
            sentry__logs_flush_crash_safe();
        }
        if (options->enable_metrics) {
            sentry__metrics_flush_crash_safe();
        }

        if (should_handle) {
            bool capture_screenshot = options->attach_screenshot;
#ifndef SENTRY_SCREENSHOT_NONE
            if (capture_screenshot && options->before_screenshot_func) {
                SENTRY_SIGNAL_SAFE_LOG(
                    "DEBUG invoking `before_screenshot` hook");
                capture_screenshot = options->before_screenshot_func(
                                         event, options->before_screenshot_data)
                    != 0;
            }
#endif

            sentry_envelope_t *envelope = sentry__prepare_event(
                options, event, nullptr, !options->on_crash_func, nullptr);
            sentry_session_t *session = sentry__end_current_session_with_status(
                SENTRY_SESSION_STATUS_CRASHED);
            sentry__envelope_add_session(envelope, session);

            // the minidump is added as an attachment,
            // with type `event.minidump`
            sentry_envelope_item_t *item = sentry__envelope_add_from_path(
                envelope, dump_path, "attachment");
            if (item) {
                sentry__envelope_item_set_header(item, "attachment_type",
                    sentry_value_new_string(SENTRY_ATTACHMENT_TYPE_MINIDUMP));

                sentry__envelope_item_set_header(item, "filename",
                    sentry_value_new_string(sentry__path_filename(dump_path)));
            } else if (options->enable_large_attachments) {
                sentry_attachment_t tmp = {};
                tmp.path = dump_path;
                tmp.type = (char *)SENTRY_ATTACHMENT_TYPE_MINIDUMP;
                if (!sentry__cache_attachment_ref(
                        envelope, &tmp, options->run->cache_path, nullptr)) {
                    SENTRY_SIGNAL_SAFE_LOG(
                        "WARN failed to cache minidump attachment-ref");
                    sentry__client_report_discard(
                        SENTRY_DISCARD_REASON_SEND_ERROR,
                        SENTRY_DATA_CATEGORY_ATTACHMENT, 1);
                }
            } else {
                SENTRY_SIGNAL_SAFE_LOG(
                    "WARN failed to add minidump attachment");
                sentry__client_report_discard(SENTRY_DISCARD_REASON_SEND_ERROR,
                    SENTRY_DATA_CATEGORY_ATTACHMENT, 1);
            }

            if (capture_screenshot) {
                sentry_attachment_t *screenshot = sentry__attachment_from_path(
                    sentry__screenshot_get_path(options));
                if (screenshot
                    && sentry__screenshot_capture(screenshot->path, 0)) {
                    sentry__envelope_add_attachment(envelope, screenshot);
                }
                sentry__attachment_free(screenshot);
            }

            if (options->attach_session_replay) {
                sentry_attachment_t *replay = sentry__attachment_from_path(
                    sentry__session_replay_get_path(options));
                if (replay
                    && sentry__session_replay_capture(
                        replay->path, options->session_replay_duration, 0)) {
                    sentry__envelope_add_attachment(envelope, replay);
                }
                sentry__attachment_free(replay);
            }

            if (envelope && sentry__session_replay_has_pending(options)) {
                sentry_value_t crash_event
                    = sentry_envelope_get_event(envelope);
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

            if (!sentry__launch_external_crash_reporter(options, envelope)) {
                // capture the envelopes with the disk transport
                sentry_transport_t *disk_transport
                    = sentry_new_disk_transport(options->run);
                if (!sentry_value_is_null(transaction)) {
                    sentry_envelope_t *tx_envelope
                        = sentry__prepare_transaction(
                            options, transaction, nullptr);
                    if (tx_envelope) {
                        sentry__capture_envelope(
                            disk_transport, tx_envelope, options);
                    }
                }
                sentry__capture_envelope(disk_transport, envelope, options);
                sentry__transport_dump_queue(disk_transport, options->run);
                sentry_transport_free(disk_transport);
            } else {
                sentry_value_decref(transaction);
            }

            // now that the envelope was written, we can remove the temporary
            // minidump file
            sentry__path_remove(dump_path);
            sentry__path_free(dump_path);
        } else {
            SENTRY_SIGNAL_SAFE_LOG(
                "DEBUG event was discarded by the `on_crash` hook");
            sentry_value_decref(event);
            sentry_value_decref(transaction);
        }

        // after capturing the crash event, try to dump all the in-flight
        // data of the previous transports
        sentry__transport_dump_queue(options->transport, options->run);
        // and restore the old transport
    }
    SENTRY_SIGNAL_SAFE_LOG("INFO crash has been captured");

#ifndef SENTRY_PLATFORM_WINDOWS
    sentry__leave_signal_handler();
#endif
    return succeeded;
}

#ifdef SENTRY_PLATFORM_MACOS
/**
 * Returns true if the current process is being debugged (either running under
 * the debugger or has a debugger attached post facto).
 */
static bool
IsDebuggerActive()
{
    int mib[4];
    kinfo_proc info;
    size_t size;

    // Initialize the flags so that, if sysctl fails for some bizarre
    // reason, we get a predictable result.
    info.kp_proc.p_flag = 0;

    // Initialize mib, which tells sysctl the info we want, in this case
    // we're looking for information about a specific process ID.
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    // Call sysctl.
    size = sizeof(info);
    [[maybe_unused]] const int junk
        = sysctl(mib, std::size(mib), &info, &size, nullptr, 0);
    assert(junk == 0);

    // We're being debugged if the P_TRACED flag is set.
    return ((info.kp_proc.p_flag & P_TRACED) != 0);
}
#endif

static int
breakpad_backend_startup(
    sentry_backend_t *backend, const sentry_options_t *options)
{
    sentry_path_t *current_run_folder = options->run->run_path;

#ifdef SENTRY_PLATFORM_WINDOWS
#    if !defined(SENTRY_BUILD_SHARED)                                          \
        && defined(SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT)
    sentry__set_default_thread_stack_guarantee();
#    endif
    backend->data = new (std::nothrow) google_breakpad::ExceptionHandler(
        current_run_folder->path_w, nullptr, breakpad_backend_callback, nullptr,
        google_breakpad::ExceptionHandler::HANDLER_EXCEPTION);
    if (backend->data) {
        sentry__win32_install_sigabrt_handler(handle_sigabrt);
    }
#elif defined(SENTRY_PLATFORM_MACOS)
    // If process is being debugged and there are breakpoints set it will cause
    // task_set_exception_ports to crash the whole process and debugger
    backend->data = new (std::nothrow)
        google_breakpad::ExceptionHandler(current_run_folder->path, nullptr,
            breakpad_backend_callback, nullptr, !IsDebuggerActive(), nullptr);
#elif defined(SENTRY_PLATFORM_IOS)
    backend->data = new (std::nothrow)
        google_breakpad::ExceptionHandler(current_run_folder->path, nullptr,
            breakpad_backend_callback, nullptr, true, nullptr);
#else
    google_breakpad::MinidumpDescriptor descriptor(current_run_folder->path);
    backend->data = new (std::nothrow) google_breakpad::ExceptionHandler(
        descriptor, nullptr, breakpad_backend_callback, nullptr, true, -1);
#endif
    return backend->data == nullptr;
}

static void
breakpad_backend_shutdown(sentry_backend_t *backend)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    sentry__win32_restore_sigabrt_handler();
#endif

    const auto *eh
        = static_cast<google_breakpad::ExceptionHandler *>(backend->data);
    backend->data = nullptr;
    delete eh;
}

static void
breakpad_backend_except(
    sentry_backend_t *backend, const sentry_ucontext_t *context)
{
    auto *eh = static_cast<google_breakpad::ExceptionHandler *>(backend->data);

#ifdef SENTRY_PLATFORM_WINDOWS
    eh->WriteMinidumpForException(
        const_cast<EXCEPTION_POINTERS *>(&context->exception_ptrs));
#elif defined(SENTRY_PLATFORM_MACOS)
    (void)context;
    eh->WriteMinidump(true);
    // currently private:
    // eh->SignalHandler(context->signum, context->siginfo,
    // context->user_context);
#elif defined(SENTRY_PLATFORM_IOS)
    // the APIs are currently private
    (void)eh;
    (void)backend;
    (void)context;
#else
    eh->HandleSignal(context->signum, context->siginfo, context->user_context);
#endif
}

extern "C" {

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

    backend->startup_func = breakpad_backend_startup;
    backend->shutdown_func = breakpad_backend_shutdown;
    backend->except_func = breakpad_backend_except;

    return backend;
}
}
