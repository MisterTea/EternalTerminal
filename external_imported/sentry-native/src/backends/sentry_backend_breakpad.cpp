extern "C" {
#include "sentry_boot.h"

#include "sentry_alloc.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_options.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_sync.h"
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
#    include "client/windows/handler/exception_handler.h"
#elif defined(SENTRY_PLATFORM_MACOS)
#    include "client/mac/handler/exception_handler.h"
#    include <sys/sysctl.h>
#elif defined(SENTRY_PLATFORM_IOS)
#    include "client/ios/exception_handler_no_mach.h"
#else
#    include "client/linux/handler/exception_handler.h"
#endif

#ifdef __GNUC__
#    pragma GCC diagnostic pop
#endif

#ifdef SENTRY_PLATFORM_WINDOWS
static bool
sentry__breakpad_backend_callback(const wchar_t *breakpad_dump_path,
    const wchar_t *minidump_id, void *UNUSED(context),
    EXCEPTION_POINTERS *UNUSED(exinfo), MDRawAssertionInfo *UNUSED(assertion),
    bool succeeded)
#elif defined(SENTRY_PLATFORM_DARWIN)
static bool
sentry__breakpad_backend_callback(const char *breakpad_dump_path,
    const char *minidump_id, void *UNUSED(context), bool succeeded)
#else
static bool
sentry__breakpad_backend_callback(
    const google_breakpad::MinidumpDescriptor &descriptor,
    void *UNUSED(context), bool succeeded)
#endif
{
    SENTRY_DEBUG("entering breakpad minidump callback");

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
    sentry__enter_signal_handler();
#endif

    sentry_path_t *dump_path = nullptr;
#ifdef SENTRY_PLATFORM_WINDOWS
    sentry_path_t *tmp_path = sentry__path_new(breakpad_dump_path);
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

    SENTRY_WITH_OPTIONS (options) {
        sentry__write_crash_marker(options);

        sentry_value_t event = sentry_value_new_event();
        sentry_envelope_t *envelope
            = sentry__prepare_event(options, event, NULL);
        // the event we just prepared is empty, so no error is recorded for it
        sentry__record_errors_on_current_session(1);
        sentry_session_t *session = sentry__end_current_session_with_status(
            SENTRY_SESSION_STATUS_CRASHED);
        sentry__envelope_add_session(envelope, session);

        // the minidump is added as an attachment, with type `event.minidump`
        sentry_envelope_item_t *item
            = sentry__envelope_add_from_path(envelope, dump_path, "attachment");
        if (item) {
            sentry__envelope_item_set_header(item, "attachment_type",
                sentry_value_new_string("event.minidump"));

            sentry__envelope_item_set_header(item, "filename",
#ifdef SENTRY_PLATFORM_WINDOWS
                sentry__value_new_string_from_wstr(
#else
                sentry_value_new_string(
#endif
                    sentry__path_filename(dump_path)));
        }

        // capture the envelope with the disk transport
        sentry_transport_t *disk_transport
            = sentry_new_disk_transport(options->run);
        sentry__capture_envelope(disk_transport, envelope);
        sentry__transport_dump_queue(disk_transport, options->run);
        sentry_transport_free(disk_transport);

        // now that the envelope was written, we can remove the temporary
        // minidump file
        sentry__path_remove(dump_path);
        sentry__path_free(dump_path);

        // after capturing the crash event, try to dump all the in-flight
        // data of the previous transports
        sentry__transport_dump_queue(options->transport, options->run);
        // and restore the old transport
    }
    SENTRY_DEBUG("crash has been captured");

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
    int junk;
    int mib[4];
    struct kinfo_proc info;
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
    junk = sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, NULL, 0);
    assert(junk == 0);

    // We're being debugged if the P_TRACED flag is set.
    return ((info.kp_proc.p_flag & P_TRACED) != 0);
}
#endif

static int
sentry__breakpad_backend_startup(
    sentry_backend_t *backend, const sentry_options_t *options)
{
    sentry_path_t *current_run_folder = options->run->run_path;

#ifdef SENTRY_PLATFORM_WINDOWS
    backend->data = new google_breakpad::ExceptionHandler(
        current_run_folder->path, NULL, sentry__breakpad_backend_callback, NULL,
        google_breakpad::ExceptionHandler::HANDLER_EXCEPTION);
#elif defined(SENTRY_PLATFORM_MACOS)
    // If process is being debugged and there are breakpoints set it will cause
    // task_set_exception_ports to crash the whole process and debugger
    backend->data
        = new google_breakpad::ExceptionHandler(current_run_folder->path, NULL,
            sentry__breakpad_backend_callback, NULL, !IsDebuggerActive(), NULL);
#elif defined(SENTRY_PLATFORM_IOS)
    backend->data
        = new google_breakpad::ExceptionHandler(current_run_folder->path, NULL,
            sentry__breakpad_backend_callback, NULL, true, NULL);
#else
    google_breakpad::MinidumpDescriptor descriptor(current_run_folder->path);
    backend->data = new google_breakpad::ExceptionHandler(
        descriptor, NULL, sentry__breakpad_backend_callback, NULL, true, -1);
#endif
    return backend->data == NULL;
}

static void
sentry__breakpad_backend_shutdown(sentry_backend_t *backend)
{
    google_breakpad::ExceptionHandler *eh
        = (google_breakpad::ExceptionHandler *)backend->data;
    backend->data = NULL;
    delete eh;
}

static void
sentry__breakpad_backend_except(
    sentry_backend_t *backend, const sentry_ucontext_t *context)
{
    google_breakpad::ExceptionHandler *eh
        = (google_breakpad::ExceptionHandler *)backend->data;

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

sentry_backend_t *
sentry__backend_new(void)
{
    sentry_backend_t *backend = SENTRY_MAKE(sentry_backend_t);
    if (!backend) {
        return NULL;
    }
    memset(backend, 0, sizeof(sentry_backend_t));

    backend->startup_func = sentry__breakpad_backend_startup;
    backend->shutdown_func = sentry__breakpad_backend_shutdown;
    backend->except_func = sentry__breakpad_backend_except;

    return backend;
}
}
