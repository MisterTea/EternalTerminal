#include "sentry_crash_context.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <werapi.h>
#include <windows.h>

#ifndef STATUS_FAIL_FAST_EXCEPTION
#    define STATUS_FAIL_FAST_EXCEPTION ((DWORD)0xC0000602)
#endif

#ifndef STATUS_STACK_BUFFER_OVERRUN
#    define STATUS_STACK_BUFFER_OVERRUN ((DWORD)0xC0000409)
#endif

static BOOL
is_fatal_wer_exception(const WER_RUNTIME_EXCEPTION_INFORMATION *info)
{
    // bIsFatal is missing in older SDKs; guard access with dwSize.
    typedef struct {
        DWORD dwSize;
        HANDLE hProcess;
        HANDLE hThread;
        EXCEPTION_RECORD exceptionRecord;
        CONTEXT context;
        PCWSTR pwszReportId;
        BOOL bIsFatal;
        DWORD dwReserved;
    } WER_RUNTIME_EXCEPTION_INFORMATION_19041;

    if (!info
        || info->dwSize
            <= offsetof(WER_RUNTIME_EXCEPTION_INFORMATION_19041, bIsFatal)) {
        return FALSE;
    }

    return ((const WER_RUNTIME_EXCEPTION_INFORMATION_19041 *)info)->bIsFatal;
}

static BOOL
is_native_wer_exception(DWORD code)
{
    return code == STATUS_FAIL_FAST_EXCEPTION
        || code == STATUS_STACK_BUFFER_OVERRUN;
}

static BOOL
read_registration(
    HANDLE process, PVOID context, sentry_wer_registration_t *registration)
{
    if (!process || !context || !registration) {
        return FALSE;
    }

    if (!ReadProcessMemory(
            process, context, registration, sizeof(*registration), NULL)) {
        return FALSE;
    }

    return registration->version == 1 && registration->app_pid != 0;
}

static BOOL
open_native_crash_objects(const sentry_wer_registration_t *registration,
    HANDLE *mapping, HANDLE *event, sentry_crash_context_t **ctx)
{
    wchar_t shm_name[SENTRY_CRASH_IPC_NAME_SIZE];
    wchar_t event_name[SENTRY_CRASH_IPC_NAME_SIZE];

    swprintf(shm_name, SENTRY_CRASH_IPC_NAME_SIZE,
        L"Local\\SentryCrash-%lu-%llx", (unsigned long)registration->app_pid,
        (unsigned long long)registration->app_tid);
    swprintf(event_name, SENTRY_CRASH_IPC_NAME_SIZE,
        L"Local\\SentryCrashEvent-%lu-%llx",
        (unsigned long)registration->app_pid,
        (unsigned long long)registration->app_tid);

    *mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
    if (!*mapping) {
        return FALSE;
    }

    *ctx = MapViewOfFile(
        *mapping, FILE_MAP_ALL_ACCESS, 0, 0, SENTRY_CRASH_SHM_SIZE);
    if (!*ctx) {
        CloseHandle(*mapping);
        *mapping = NULL;
        return FALSE;
    }

    if ((*ctx)->magic != SENTRY_CRASH_MAGIC) {
        UnmapViewOfFile(*ctx);
        CloseHandle(*mapping);
        *ctx = NULL;
        *mapping = NULL;
        return FALSE;
    }

    *event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);
    if (!*event) {
        UnmapViewOfFile(*ctx);
        CloseHandle(*mapping);
        *ctx = NULL;
        *mapping = NULL;
        return FALSE;
    }

    return TRUE;
}

static BOOL
process_wer_exception(
    PVOID context, const WER_RUNTIME_EXCEPTION_INFORMATION *exception_info)
{
    if (!exception_info || !is_fatal_wer_exception(exception_info)
        || !is_native_wer_exception(
            exception_info->exceptionRecord.ExceptionCode)) {
        return FALSE;
    }

    sentry_wer_registration_t registration = { 0 };
    if (!read_registration(exception_info->hProcess, context, &registration)) {
        return FALSE;
    }

    HANDLE mapping = NULL;
    HANDLE event = NULL;
    sentry_crash_context_t *ctx = NULL;
    if (!open_native_crash_objects(&registration, &mapping, &event, &ctx)) {
        return FALSE;
    }

    BOOL claimed = FALSE;
    if (InterlockedCompareExchange(&ctx->state, SENTRY_CRASH_STATE_PROCESSING,
            SENTRY_CRASH_STATE_READY)
        == SENTRY_CRASH_STATE_READY) {
        ctx->crashed_pid = GetProcessId(exception_info->hProcess);
        ctx->crashed_tid = GetThreadId(exception_info->hThread);
        ctx->platform.exception_code
            = exception_info->exceptionRecord.ExceptionCode;
        ctx->platform.exception_record = exception_info->exceptionRecord;
        ctx->platform.context = exception_info->context;
        ctx->platform.exception_pointers = NULL;
        ctx->platform.num_threads = 1;
        ctx->platform.threads[0].thread_id = ctx->crashed_tid;
        ctx->platform.threads[0].context = exception_info->context;

        InterlockedExchange(&ctx->state, SENTRY_CRASH_STATE_CRASHED);
        if (SetEvent(event)) {
            claimed = TRUE;
            uint64_t timeout_ms = ctx->shutdown_timeout
                ? ctx->shutdown_timeout
                : SENTRY_CRASH_HANDLER_WAIT_TIMEOUT_MS;
            for (uint64_t waited_ms = 0; waited_ms < timeout_ms;
                waited_ms += SENTRY_CRASH_HANDLER_POLL_INTERVAL_MS) {
                if (InterlockedCompareExchange(&ctx->state,
                        SENTRY_CRASH_STATE_DONE, SENTRY_CRASH_STATE_DONE)
                    >= SENTRY_CRASH_STATE_CAPTURED) {
                    break;
                }
                Sleep(SENTRY_CRASH_HANDLER_POLL_INTERVAL_MS);
            }
            TerminateProcess(exception_info->hProcess,
                exception_info->exceptionRecord.ExceptionCode);
        }
    }

    CloseHandle(event);
    UnmapViewOfFile(ctx);
    CloseHandle(mapping);
    return claimed;
}

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

HRESULT WINAPI
OutOfProcessExceptionEventCallback(PVOID context,
    const PWER_RUNTIME_EXCEPTION_INFORMATION exception_info,
    BOOL *ownership_claimed, PWSTR event_name, PDWORD event_name_size,
    PDWORD signature_count)
{
    (void)event_name;
    (void)event_name_size;
    (void)signature_count;

    *ownership_claimed = FALSE;
    if (process_wer_exception(context, exception_info)) {
        *ownership_claimed = TRUE;
    }
    return S_OK;
}

HRESULT WINAPI
OutOfProcessExceptionEventSignatureCallback(PVOID context,
    const PWER_RUNTIME_EXCEPTION_INFORMATION exception_info, DWORD index,
    PWSTR name, PDWORD name_size, PWSTR value, PDWORD value_size)
{
    (void)context;
    (void)exception_info;
    (void)index;
    (void)name;
    (void)name_size;
    (void)value;
    (void)value_size;
    return E_FAIL;
}

HRESULT WINAPI
OutOfProcessExceptionEventDebuggerLaunchCallback(PVOID context,
    const PWER_RUNTIME_EXCEPTION_INFORMATION exception_info,
    PBOOL is_custom_debugger, PWSTR debugger_launch,
    PDWORD debugger_launch_size, PBOOL is_debugger_autolaunch)
{
    (void)context;
    (void)exception_info;
    (void)is_custom_debugger;
    (void)debugger_launch;
    (void)debugger_launch_size;
    (void)is_debugger_autolaunch;
    return E_FAIL;
}
