#include "sentry_boot.h"

#include "sentry_symbolizer.h"
#include "sentry_windows_dbghelp.h"

#include <dbghelp.h>
#include <malloc.h>

#define MAX_SYM 1024

bool
sentry__symbolize(
    void *addr, void (*func)(const sentry_frame_info_t *, void *), void *data)
{
    HANDLE proc = sentry__init_dbghelp();

    SYMBOL_INFO *sym = (SYMBOL_INFO *)_alloca(sizeof(SYMBOL_INFO) + MAX_SYM);
    memset(sym, 0, sizeof(SYMBOL_INFO) + MAX_SYM);
    sym->MaxNameLen = MAX_SYM;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);

    if (!SymFromAddr(proc, (DWORD64)addr, 0, sym)) {
        return false;
    }

    char mod_name[MAX_PATH];
    GetModuleFileNameA(
        (HMODULE)(size_t)sym->ModBase, mod_name, sizeof(mod_name));

    sentry_frame_info_t frame_info;
    memset(&frame_info, 0, sizeof(sentry_frame_info_t));
    frame_info.load_addr = (void *)(size_t)sym->ModBase;
    frame_info.instruction_addr = addr;
    frame_info.symbol_addr = (void *)(size_t)sym->Address;
    frame_info.symbol = sym->Name;
    frame_info.object_name = mod_name;
    func(&frame_info, data);

    return true;
}
