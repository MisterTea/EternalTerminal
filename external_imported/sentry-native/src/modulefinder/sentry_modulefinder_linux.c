#include "sentry_modulefinder_linux.h"

#include "sentry_core.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_value.h"

#include <arpa/inet.h>
#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static bool g_initialized = false;
static sentry_mutex_t g_mutex = SENTRY__MUTEX_INIT;
static sentry_value_t g_modules = { 0 };

static sentry_slice_t LINUX_GATE = { "linux-gate.so", 13 };

int
sentry__procmaps_parse_module_line(const char *line, sentry_module_t *module)
{
    char permissions[5] = { 0 };
    uint64_t offset;
    uint8_t major_device;
    uint8_t minor_device;
    uint64_t inode;
    int consumed = 0;

    // this has been copied from the breakpad source:
    // https://github.com/google/breakpad/blob/13c1568702e8804bc3ebcfbb435a2786a3e335cf/src/processor/proc_maps_linux.cc#L66
    if (sscanf(line,
            "%" SCNxPTR "-%" SCNxPTR " %4c %" SCNx64 " %hhx:%hhx %" SCNu64
            " %n",
            (uintptr_t *)&module->start, (uintptr_t *)&module->end, permissions,
            &offset, &major_device, &minor_device, &inode, &consumed)
        < 7) {
        return 0;
    }

    // copy the filename up to a newline
    line += consumed;
    module->file.ptr = line;
    module->file.len = 0;
    char *nl = strchr(line, '\n');
    // `consumed` skips over whitespace (the trailing newline), so we have to
    // check for that explicitly
    if (consumed && (line - 1)[0] == '\n') {
        module->file.ptr = NULL;
    } else if (nl) {
        module->file.len = nl - line;
        consumed += nl - line + 1;
    } else {
        module->file.len = strlen(line);
        consumed += module->file.len;
    }

    // and return the consumed chars…
    return consumed;
}

bool
sentry__mmap_file(sentry_mmap_t *rv, const char *path)
{
    rv->fd = open(path, O_RDONLY);
    if (rv->fd < 0) {
        goto fail;
    }

    struct stat sb;
    if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        goto fail;
    }

    rv->len = sb.st_size;
    if (rv->len == 0) {
        goto fail;
    }

    rv->ptr = mmap(NULL, rv->len, PROT_READ, MAP_PRIVATE, rv->fd, 0);
    if (rv->ptr == MAP_FAILED) {
        goto fail;
    }

    return true;

fail:
    if (rv->fd > 0) {
        close(rv->fd);
    }
    rv->fd = 0;
    rv->ptr = NULL;
    rv->len = 0;
    return false;

    return rv;
}

void
sentry__mmap_close(sentry_mmap_t *m)
{
    munmap(m->ptr, m->len);
    close(m->fd);
    m->ptr = NULL;
    m->len = 0;
    m->fd = 0;
}

void
align(size_t alignment, void **offset)
{
    size_t diff = (size_t)*offset % alignment;
    if (diff != 0) {
        *(size_t *)offset += alignment - diff;
    }
}

static const uint8_t *
get_code_id_from_notes(
    size_t alignment, void *start, void *end, size_t *size_out)
{
    *size_out = 0;
    if (alignment < 4) {
        alignment = 4;
    } else if (alignment != 4 && alignment != 8) {
        return NULL;
    }

    const uint8_t *offset = start;
    while (offset < (const uint8_t *)end) {
        // The note header size is independant of the architecture, so we just
        // use the `Elf64_Nhdr` variant.
        const Elf64_Nhdr *note = (const Elf64_Nhdr *)offset;
        // the headers are consecutive, and the optional `name` and `desc` are
        // saved inline after the header.

        offset += sizeof(Elf64_Nhdr);
        offset += note->n_namesz;
        align(alignment, (void **)&offset);
        if (note->n_type == NT_GNU_BUILD_ID) {
            *size_out = note->n_descsz;
            return offset;
        }
        offset += note->n_descsz;
        align(alignment, (void **)&offset);
    }
    return NULL;
}

static bool
is_elf_module(void *addr)
{
    // we try to interpret `addr` as an ELF file, which should start with a
    // magic number...
    const unsigned char *e_ident = addr;
    return e_ident[EI_MAG0] == ELFMAG0 && e_ident[EI_MAG1] == ELFMAG1
        && e_ident[EI_MAG2] == ELFMAG2 && e_ident[EI_MAG3] == ELFMAG3;
}

static const uint8_t *
get_code_id_from_elf(void *base, size_t *size_out)
{
    *size_out = 0;

    // now this is interesting:
    // `p_offset` is defined as the offset of the section relative to the file,
    // and `p_vaddr` is supposed to be the memory location.
    // interestingly though, when compiled with gcc 7.4, both are the same,
    // because apparently it does not really patch up the `p_vaddr`. gcc 5.4
    // however does, so `p_vaddr` is an actual pointer, and not an offset to
    // be added to the `base`. So we are using `p_offset` here, since it seems
    // to be the correct offset relative to `base` using both compilers.
    const uint8_t *addr = base;

    // iterate over all the program headers, for 32/64 bit separately
    const unsigned char *e_ident = addr;
    if (e_ident[EI_CLASS] == ELFCLASS64) {
        const Elf64_Ehdr *elf = base;
        for (int i = 0; i < elf->e_phnum; i++) {
            const Elf64_Phdr *header = (const Elf64_Phdr *)(addr + elf->e_phoff
                + elf->e_phentsize * i);
            // we are only interested in notes
            if (header->p_type != PT_NOTE) {
                continue;
            }
            const uint8_t *code_id = get_code_id_from_notes(header->p_align,
                (void *)(addr + header->p_offset),
                (void *)(addr + header->p_offset + header->p_memsz), size_out);
            if (code_id) {
                return code_id;
            }
        }
    } else {
        const Elf32_Ehdr *elf = base;
        for (int i = 0; i < elf->e_phnum; i++) {
            const Elf32_Phdr *header = (const Elf32_Phdr *)(addr + elf->e_phoff
                + elf->e_phentsize * i);
            // we are only interested in notes
            if (header->p_type != PT_NOTE) {
                continue;
            }
            const uint8_t *code_id = get_code_id_from_notes(header->p_align,
                (void *)(addr + header->p_offset),
                (void *)(addr + header->p_offset + header->p_memsz), size_out);
            if (code_id) {
                return code_id;
            }
        }
    }
    return NULL;
}

static sentry_uuid_t
get_code_id_from_text_fallback(void *base)
{
    const uint8_t *text = NULL;
    size_t text_size = 0;

    const uint8_t *addr = base;
    // iterate over all the program headers, for 32/64 bit separately
    const unsigned char *e_ident = addr;
    if (e_ident[EI_CLASS] == ELFCLASS64) {
        const Elf64_Ehdr *elf = base;
        const Elf64_Shdr *strheader = (const Elf64_Shdr *)(addr + elf->e_shoff
            + elf->e_shentsize * elf->e_shstrndx);

        const char *names = (const char *)(addr + strheader->sh_offset);
        for (int i = 0; i < elf->e_shnum; i++) {
            const Elf64_Shdr *header = (const Elf64_Shdr *)(addr + elf->e_shoff
                + elf->e_shentsize * i);
            const char *name = names + header->sh_name;
            if (header->sh_type == SHT_PROGBITS && strcmp(name, ".text") == 0) {
                text = addr + header->sh_offset;
                text_size = header->sh_size;
                break;
            }
        }
    } else {
        const Elf32_Ehdr *elf = base;
        const Elf32_Shdr *strheader = (const Elf32_Shdr *)(addr + elf->e_shoff
            + elf->e_shentsize * elf->e_shstrndx);

        const char *names = (const char *)(addr + strheader->sh_offset);
        for (int i = 0; i < elf->e_shnum; i++) {
            const Elf32_Shdr *header = (const Elf32_Shdr *)(addr + elf->e_shoff
                + elf->e_shentsize * i);
            const char *name = names + header->sh_name;
            if (header->sh_type == SHT_PROGBITS && strcmp(name, ".text") == 0) {
                text = addr + header->sh_offset;
                text_size = header->sh_size;
                break;
            }
        }
    }

    sentry_uuid_t uuid = sentry_uuid_nil();

    // adapted from
    // https://github.com/getsentry/symbolic/blob/8f9a01756e48dcbba2e42917a064f495d74058b7/debuginfo/src/elf.rs#L100-L110
    size_t max = MIN(text_size, 4096);
    for (size_t i = 0; i < max; i++) {
        uuid.bytes[i % 16] ^= text[i];
    }

    return uuid;
}

bool
sentry__procmaps_read_ids_from_elf(sentry_value_t value, void *elf_ptr)
{
    // and try to get the debug id from the elf headers of the loaded
    // modules
    size_t code_id_size;
    const uint8_t *code_id = get_code_id_from_elf(elf_ptr, &code_id_size);
    sentry_uuid_t uuid = sentry_uuid_nil();
    if (code_id) {
        sentry_value_set_by_key(value, "code_id",
            sentry__value_new_hexstring(code_id, code_id_size));

        memcpy(uuid.bytes, code_id, MIN(code_id_size, 16));
    } else {
        uuid = get_code_id_from_text_fallback(elf_ptr);
    }

    // the usage of these is described here:
    // https://getsentry.github.io/symbolicator/advanced/symbol-server-compatibility/#identifiers
    // in particular, the debug_id is a `little-endian GUID`, so we
    // have to do appropriate byte-flipping
    char *uuid_bytes = uuid.bytes;
    uint32_t *a = (uint32_t *)uuid_bytes;
    *a = htonl(*a);
    uint16_t *b = (uint16_t *)(uuid_bytes + 4);
    *b = htons(*b);
    uint16_t *c = (uint16_t *)(uuid_bytes + 6);
    *c = htons(*c);

    sentry_value_set_by_key(value, "debug_id", sentry__value_new_uuid(&uuid));
    return true;
}

sentry_value_t
sentry__procmaps_module_to_value(const sentry_module_t *module)
{
    sentry_value_t mod_val = sentry_value_new_object();
    sentry_value_set_by_key(mod_val, "type", sentry_value_new_string("elf"));
    sentry_value_set_by_key(mod_val, "image_addr",
        sentry__value_new_addr((uint64_t)(size_t)module->start));
    sentry_value_set_by_key(mod_val, "image_size",
        sentry_value_new_int32(
            (int32_t)((size_t)module->end - (size_t)module->start)));
    sentry_value_set_by_key(mod_val, "code_file",
        sentry__value_new_string_owned(sentry__slice_to_owned(module->file)));

    // At least on the android API-16, x86 simulator, the linker apparently
    // does not load the complete file into memory. Or at least, the section
    // headers which are located at the end of the file are not loaded, and
    // we would be poking into invalid memory. To be safe, we mmap the complete
    // file from disk, so we have the on-disk layout, and are independent of how
    // the runtime linker would load or re-order any sections. The exception
    // here is the linux-gate, which is not an actual file on disk, so we
    // actually poke at its memory.
    if (sentry__slice_eq(module->file, LINUX_GATE)) {
        if (!is_elf_module(module->start)) {
            goto fail;
        }
        sentry__procmaps_read_ids_from_elf(mod_val, module->start);
    } else {
        char *filename = sentry__slice_to_owned(module->file);
        sentry_mmap_t mm;
        if (!sentry__mmap_file(&mm, filename)) {
            sentry_free(filename);
            goto fail;
        }
        sentry_free(filename);

        if (!is_elf_module(mm.ptr)) {
            sentry__mmap_close(&mm);
            goto fail;
        }

        sentry__procmaps_read_ids_from_elf(mod_val, mm.ptr);

        sentry__mmap_close(&mm);
    }

    return mod_val;

fail:
    sentry_value_decref(mod_val);
    return sentry_value_new_null();
}

static void
try_append_module(sentry_value_t modules, const sentry_module_t *module)
{
    if (!module->file.ptr) {
        return;
    }

    SENTRY_TRACEF(
        "inspecting module \"%.*s\"", (int)module->file.len, module->file.ptr);
    sentry_value_t mod_val = sentry__procmaps_module_to_value(module);
    if (!sentry_value_is_null(mod_val)) {
        sentry_value_append(modules, mod_val);
    }
}

// copied from:
// https://github.com/google/breakpad/blob/216cea7bca53fa441a3ee0d0f5fd339a3a894224/src/client/linux/minidump_writer/linux_dumper.h#L61-L70
#if defined(__i386) || defined(__ARM_EABI__)                                   \
    || (defined(__mips__) && _MIPS_SIM == _ABIO32)
typedef Elf32_auxv_t elf_aux_entry;
#elif defined(__x86_64) || defined(__aarch64__)                                \
    || (defined(__mips__) && _MIPS_SIM != _ABIO32)
typedef Elf64_auxv_t elf_aux_entry;
#endif

// See http://man7.org/linux/man-pages/man7/vdso.7.html
static void *
get_linux_vdso(void)
{
    // this is adapted from:
    // https://github.com/google/breakpad/blob/79ba6a494fb2097b39f76fe6a4b4b4f407e32a02/src/client/linux/minidump_writer/linux_dumper.cc#L548-L572

    int fd = open("/proc/self/auxv", O_RDONLY);
    if (fd < 0) {
        return false;
    }

    elf_aux_entry one_aux_entry;
    while (
        read(fd, &one_aux_entry, sizeof(elf_aux_entry)) == sizeof(elf_aux_entry)
        && one_aux_entry.a_type != AT_NULL) {
        if (one_aux_entry.a_type == AT_SYSINFO_EHDR) {
            close(fd);
            return (void *)one_aux_entry.a_un.a_val;
        }
    }
    close(fd);
    return NULL;
}

static void
load_modules(sentry_value_t modules)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        return;
    }

    // just read the whole map at once, maybe do it line-by-line as a followup…
    char buf[4096];
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    while (true) {
        ssize_t n = read(fd, buf, 4096);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        } else if (n <= 0) {
            break;
        }
        if (sentry__stringbuilder_append_buf(&sb, buf, n)) {
            sentry__stringbuilder_cleanup(&sb);
            close(fd);
            return;
        }
    }
    close(fd);

    char *contents = sentry__stringbuilder_into_string(&sb);
    if (!contents) {
        return;
    }
    char *current_line = contents;

    void *linux_vdso = get_linux_vdso();

    // we have multiple memory maps per file, and we need to merge their offsets
    // based on the filename. Luckily, the maps are ordered by filename, so yay
    sentry_module_t last_module = { (void *)-1, 0, { NULL, 0 } };
    while (true) {
        sentry_module_t module = { 0, 0, { NULL, 0 } };
        int read = sentry__procmaps_parse_module_line(current_line, &module);
        current_line += read;
        if (!read) {
            break;
        }

        // for the vdso, we use the special filename `linux-gate.so`,
        // otherwise we check that we have a valid pathname (with a `/` inside),
        // and skip over things that end in `)`, because entries marked as
        // `(deleted)` might crash when dereferencing, trying to check if its
        // a valid elf file.
        char *slash;
        if (module.start && module.start == linux_vdso) {
            module.file = LINUX_GATE;
        } else if (!module.start || !module.file.len
            || module.file.ptr[module.file.len - 1] == ')'
            || (slash = strchr(module.file.ptr, '/')) == NULL
            || slash > module.file.ptr + module.file.len
            || (module.file.len >= 5
                && memcmp("/dev/", module.file.ptr, 5) == 0)) {
            continue;
        }

        if (last_module.file.len
            && !sentry__slice_eq(last_module.file, module.file)) {
            try_append_module(modules, &last_module);
            last_module = module;
        } else {
            // otherwise merge it
            last_module.start = MIN(last_module.start, module.start);
            last_module.end = MAX(last_module.end, module.end);
            last_module.file = module.file;
        }
    }
    try_append_module(modules, &last_module);
    sentry_free(contents);
}

sentry_value_t
sentry_get_modules_list(void)
{
    sentry__mutex_lock(&g_mutex);
    if (!g_initialized) {
        g_modules = sentry_value_new_list();
        SENTRY_TRACE("trying to read modules from /proc/self/maps");
        load_modules(g_modules);
        SENTRY_TRACEF("read %zu modules from /proc/self/maps",
            sentry_value_get_length(g_modules));
        sentry_value_freeze(g_modules);
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
