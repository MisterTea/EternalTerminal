#include "sentry_windows_dbghelp.h"

#include "sentry_sync.h"

#include <dbghelp.h>

static sentry_mutex_t g_sym_mutex = SENTRY__MUTEX_INIT;
static bool g_initialized = false;
static HANDLE g_proc = INVALID_HANDLE_VALUE;

HANDLE
sentry__init_dbghelp(void)
{
    sentry__mutex_lock(&g_sym_mutex);
    if (!g_initialized) {
        DWORD options = SymGetOptions();
        SymSetOptions(options | SYMOPT_UNDNAME);
        g_proc = GetCurrentProcess();
        SymInitialize(g_proc, NULL, TRUE);
        g_initialized = true;
    }
    sentry__mutex_unlock(&g_sym_mutex);
    return g_proc;
}
