#include "sentry_boot.h"

#include "sentry_sync.h"
#include "sentry_uuid.h"
#include "sentry_value.h"

#include <dbghelp.h>
#include <tlhelp32.h>

static bool g_initialized = false;
static sentry_mutex_t g_mutex = SENTRY__MUTEX_INIT;
static sentry_value_t g_modules = { 0 };

#define CV_SIGNATURE 0x53445352

struct CodeViewRecord70 {
    uint32_t signature;
    GUID pdb_signature;
    uint32_t pdb_age;
    char pdb_filename[1];
};

static void
extract_pdb_info(uintptr_t module_addr, sentry_value_t module)
{
    if (!module_addr) {
        return;
    }

    PIMAGE_DOS_HEADER dos_header = (PIMAGE_DOS_HEADER)module_addr;
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }

    PIMAGE_NT_HEADERS nt_headers
        = (PIMAGE_NT_HEADERS)(module_addr + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    uint32_t relative_addr
        = nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]
              .VirtualAddress;
    if (!relative_addr) {
        return;
    }

    PIMAGE_DEBUG_DIRECTORY debug_dict
        = (PIMAGE_DEBUG_DIRECTORY)(module_addr + relative_addr);
    if (!debug_dict || debug_dict->Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
        return;
    }

    struct CodeViewRecord70 *debug_info
        = (struct CodeViewRecord70 *)(module_addr
            + debug_dict->AddressOfRawData);
    if (!debug_info || debug_info->signature != CV_SIGNATURE) {
        return;
    }

    sentry_value_set_by_key(module, "debug_file",
        sentry_value_new_string(debug_info->pdb_filename));

    sentry_uuid_t debug_id_base
        = sentry__uuid_from_native(&debug_info->pdb_signature);
    char id_buf[50];
    sentry_uuid_as_string(&debug_id_base, id_buf);
    id_buf[36] = '-';
    snprintf(id_buf + 37, 10, "%x", debug_info->pdb_age);
    sentry_value_set_by_key(
        module, "debug_id", sentry_value_new_string(id_buf));

    snprintf(id_buf, sizeof(id_buf), "%08x%X",
        nt_headers->FileHeader.TimeDateStamp,
        nt_headers->OptionalHeader.SizeOfImage);
    sentry_value_set_by_key(module, "code_id", sentry_value_new_string(id_buf));
    sentry_value_set_by_key(module, "type", sentry_value_new_string("pe"));
}

static void
load_modules(void)
{
    HANDLE snapshot
        = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32W module = { 0 };
    module.dwSize = sizeof(MODULEENTRY32W);
    g_modules = sentry_value_new_list();

    if (Module32FirstW(snapshot, &module)) {
        do {
            HMODULE handle = LoadLibraryExW(
                module.szExePath, NULL, LOAD_LIBRARY_AS_DATAFILE);
            MEMORY_BASIC_INFORMATION vmem_info = { 0 };
            if (handle
                && sizeof(vmem_info)
                    == VirtualQuery(
                        module.modBaseAddr, &vmem_info, sizeof(vmem_info))
                && vmem_info.State == MEM_COMMIT) {
                sentry_value_t rv = sentry_value_new_object();
                sentry_value_set_by_key(rv, "image_addr",
                    sentry__value_new_addr((uint64_t)module.modBaseAddr));
                sentry_value_set_by_key(rv, "image_size",
                    sentry_value_new_int32((int32_t)module.modBaseSize));
                sentry_value_set_by_key(rv, "code_file",
                    sentry__value_new_string_from_wstr(module.szExePath));
                extract_pdb_info((uintptr_t)module.modBaseAddr, rv);
                sentry_value_append(g_modules, rv);
            }
            FreeLibrary(handle);
        } while (Module32NextW(snapshot, &module));
    }

    CloseHandle(snapshot);

    sentry_value_freeze(g_modules);
}

sentry_value_t
sentry_get_modules_list(void)
{
    sentry__mutex_lock(&g_mutex);
    if (!g_initialized) {
        load_modules();
        g_initialized = true;
    }
    sentry_value_t modules = g_modules;
    sentry_value_incref(modules);
    sentry__mutex_unlock(&g_mutex);
    return modules;
}

void
sentry_clear_modulecache(void)
{
    sentry__mutex_lock(&g_mutex);
    sentry_value_decref(g_modules);
    g_modules = sentry_value_new_null();
    g_initialized = false;
    sentry__mutex_unlock(&g_mutex);
}
