#include "sentry_boot.h"

#if defined(SENTRY_PLATFORM_WINDOWS)

#    include <dbghelp.h>
#    include <stdio.h>
#    include <windows.h>

#    include "sentry.h"
#    include "sentry_logger.h"
#    include "sentry_minidump_writer.h"
#    include "sentry_string.h"

/**
 * On Xbox, MiniDumpWriteDump is provided by xgameplatform.lib
 * (linked by the Xbox toolchain), not dbghelp.lib.
 */
#    if !defined(SENTRY_PLATFORM_XBOX)
#        pragma comment(lib, "dbghelp.lib")
#    endif

/**
 * Windows minidump writer
 * Windows provides MiniDumpWriteDump API which does all the heavy lifting!
 */
int
sentry__write_minidump(
    const sentry_crash_context_t *ctx, const char *output_path)
{
    SENTRY_DEBUGF("writing minidump to %s", output_path);

    // Open output file - use wide character API for proper UTF-8 path support
    wchar_t *woutput_path = sentry__string_to_wstr(output_path);
    if (!woutput_path) {
        SENTRY_WARN("failed to convert minidump path to wide string");
        return -1;
    }

    HANDLE file_handle = CreateFileW(woutput_path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file_handle == INVALID_HANDLE_VALUE) {
        SENTRY_WARNF("failed to create minidump file: %lu", GetLastError());
        sentry_free(woutput_path);
        return -1;
    }
    sentry_free(woutput_path);

    // Open crashed process with permissions needed for MiniDumpWriteDump.
    // FULL mode uses MiniDumpWithHandleData | MiniDumpWithThreadInfo which
    // requires PROCESS_DUP_HANDLE.
    DWORD access_flags = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
    if (ctx->minidump_mode == SENTRY_MINIDUMP_MODE_FULL) {
        access_flags |= PROCESS_DUP_HANDLE;
    }
    HANDLE process_handle = OpenProcess(access_flags, FALSE, ctx->crashed_pid);

    if (process_handle == NULL) {
        SENTRY_WARNF("failed to open process %lu: %lu", ctx->crashed_pid,
            GetLastError());
        CloseHandle(file_handle);
        wchar_t *wdelete_path = sentry__string_to_wstr(output_path);
        if (wdelete_path) {
            DeleteFileW(wdelete_path);
            sentry_free(wdelete_path);
        }
        return -1;
    }

    // Prepare exception information using original pointers from crashed
    // process
    EXCEPTION_POINTERS local_exception_pointers = { 0 };
    EXCEPTION_RECORD local_exception_record = { 0 };
    MINIDUMP_EXCEPTION_INFORMATION exception_info = { 0 };
    exception_info.ThreadId = ctx->crashed_tid;
    if (ctx->platform.exception_pointers) {
        // Use original exception pointers from crashed process's address space
        exception_info.ExceptionPointers = ctx->platform.exception_pointers;
        // ClientPointers=TRUE tells Windows these pointers are in the target
        // process
        exception_info.ClientPointers = TRUE;
    } else {
        // WER copies exception data into shared memory, so ClientPointers must
        // be false when using the copied record/context.
        local_exception_record = ctx->platform.exception_record;
        local_exception_record.ExceptionRecord = NULL;
        local_exception_pointers.ExceptionRecord = &local_exception_record;
        local_exception_pointers.ContextRecord
            = (PCONTEXT)&ctx->platform.context;
        exception_info.ExceptionPointers = &local_exception_pointers;
        exception_info.ClientPointers = FALSE;
    }

    // Determine minidump type based on configuration
    MINIDUMP_TYPE dump_type;
    switch (ctx->minidump_mode) {
    case SENTRY_MINIDUMP_MODE_STACK_ONLY:
        dump_type = MiniDumpNormal;
        break;

    case SENTRY_MINIDUMP_MODE_SMART:
        dump_type
            = MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs;
        break;

    case SENTRY_MINIDUMP_MODE_FULL:
        dump_type = MiniDumpWithFullMemory | MiniDumpWithHandleData
            | MiniDumpWithThreadInfo;
        break;

    default:
        dump_type = MiniDumpNormal;
        break;
    }

    // Write minidump using Windows API
    BOOL success = MiniDumpWriteDump(process_handle, ctx->crashed_pid,
        file_handle, dump_type, &exception_info, NULL, NULL);

    DWORD error = GetLastError();

    CloseHandle(process_handle);
    CloseHandle(file_handle);

    if (!success) {
        SENTRY_WARNF("MiniDumpWriteDump failed: %lu", error);
        wchar_t *wdelete_path2 = sentry__string_to_wstr(output_path);
        if (wdelete_path2) {
            DeleteFileW(wdelete_path2);
            sentry_free(wdelete_path2);
        }
        return -1;
    }

    SENTRY_DEBUG("successfully wrote minidump");
    return 0;
}

#endif // SENTRY_PLATFORM_WINDOWS
