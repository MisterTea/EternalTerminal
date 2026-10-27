#include "sentry_boot.h"
#include "sentry_string.h"

#include "sentry_symbolizer.h"
#include "sentry_windows_dbghelp.h"

#include <dbghelp.h>
#include <malloc.h>

// follow the maximum path length documented here:
// https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
#define MAX_PATH_BUFFER_SIZE 32768

bool
sentry__symbolize(
    void *addr, void (*func)(const sentry_frame_info_t *, void *), void *data)
{
#ifdef SENTRY_PLATFORM_XBOX
    (void)data;
    (void)func;
    (void)addr;
#else
    if (!addr || !func) {
        return false;
    }
    HANDLE proc = sentry__init_dbghelp();
    size_t symbol_info_size
        = sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR);
    SYMBOL_INFOW *symbol_info = _alloca(symbol_info_size);
    memset(symbol_info, 0, symbol_info_size);
    symbol_info->MaxNameLen = MAX_SYM_NAME;
    symbol_info->SizeOfStruct = sizeof(SYMBOL_INFOW);

    if (!SymFromAddrW(proc, (uintptr_t)addr, NULL, symbol_info)) {
        return false;
    }

    wchar_t *mod_path_w = sentry_malloc(sizeof(wchar_t) * MAX_PATH_BUFFER_SIZE);
    if (!mod_path_w) {
        return false;
    }
    const DWORD n = GetModuleFileNameW((HMODULE)(uintptr_t)symbol_info->ModBase,
        mod_path_w, MAX_PATH_BUFFER_SIZE);
    if (n == 0 || n >= MAX_PATH_BUFFER_SIZE) {
        sentry_free(mod_path_w);
        return false;
    }

    char *mod_path = sentry__string_from_wstr(mod_path_w);
    char *symbol_name = sentry__string_from_wstr(symbol_info->Name);

    sentry_frame_info_t frame_info = { 0 };
    frame_info.load_addr = (void *)(uintptr_t)symbol_info->ModBase;
    frame_info.instruction_addr = addr;
    frame_info.symbol_addr = (void *)(uintptr_t)symbol_info->Address;
    frame_info.symbol = symbol_name;
    frame_info.object_name = mod_path;
    func(&frame_info, data);

    sentry_free(mod_path);
    sentry_free(symbol_name);
    sentry_free(mod_path_w);
#endif // SENTRY_PLATFORM_XBOX

    return true;
}
