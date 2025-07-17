#ifndef _GNU_SOURCE
#    define _GNU_SOURCE 1
#endif
#include "sentry_modulefinder_linux.h"

#include "sentry_core.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_utils.h"
#include "sentry_value.h"

#include <arpa/inet.h>
#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__ANDROID_API__) && __ANDROID_API__ < 23
static ssize_t
process_vm_readv(pid_t __pid, const struct iovec *__local_iov,
    unsigned long __local_iov_count, const struct iovec *__remote_iov,
    unsigned long __remote_iov_count, unsigned long __flags)
{
    return syscall(__NR_process_vm_readv, __pid, __local_iov, __local_iov_count,
        __remote_iov, __remote_iov_count, __flags);
}
#endif

#define ENSURE(Ptr)                                                            \
    if (!Ptr)                                                                  \
    goto fail

static bool g_initialized = false;
#ifdef SENTRY__MUTEX_INIT_DYN
SENTRY__MUTEX_INIT_DYN(g_mutex)
#else
static sentry_mutex_t g_mutex = SENTRY__MUTEX_INIT;
#endif
static sentry_value_t g_modules = { 0 };

static sentry_slice_t LINUX_GATE = { "linux-gate.so", 13 };

bool
sentry__mmap_file(sentry_mmap_t *rv, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
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

    rv->ptr = mmap(NULL, rv->len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (rv->ptr == MAP_FAILED) {
        goto fail;
    }

    close(fd);

    return true;

fail:
    if (fd > 0) {
        close(fd);
    }
    rv->ptr = NULL;
    rv->len = 0;
    return false;
}

void
sentry__mmap_close(sentry_mmap_t *m)
{
    munmap(m->ptr, m->len);
    m->ptr = NULL;
    m->len = 0;
}

/**
 * Checks that `start_offset` + `size` is a valid contiguous mapping in the
 * mapped regions, and returns the translated pointer corresponding to
 * `start_offset`.
 */
void *
sentry__module_get_addr(
    const sentry_module_t *module, uint64_t start_offset, uint64_t size)
{
    for (size_t i = 0; i < module->num_mappings; i++) {
        const sentry_mapped_region_t *mapping = &module->mappings[i];
        uint64_t mapping_offset = mapping->offset - module->offset_in_inode;

        // start_offset is inside this mapping
        if (start_offset >= mapping_offset
            && start_offset < mapping_offset + mapping->size) {
            uint64_t addr = start_offset - mapping_offset + mapping->addr;
            // the requested size is fully inside the mapping
            if (addr + size <= mapping->addr + mapping->size) {
                return (void *)(uintptr_t)(addr);
            }
        }
    }
    return NULL;
}

/**
 * Reads `size` bytes from `src` to `dst` safely without segfaulting in case
 * `src` is not readable.
 */
static bool
read_safely(void *dst, void *src, size_t size)
{
    struct iovec local[1];
    struct iovec remote[1];

    pid_t pid = getpid();
    local[0].iov_base = dst;
    local[0].iov_len = size;
    remote[0].iov_base = src;
    remote[0].iov_len = size;

    errno = 0;
    ssize_t nread = process_vm_readv(pid, local, 1, remote, 1, 0);
    bool rv = nread == (ssize_t)size;

    // The syscall can fail with `EPERM` if we lack permissions for this syscall
    // (which is the case when running in Docker for example,
    // See https://github.com/getsentry/sentry-native/issues/578).
    // Also, the syscall is only available in Linux 3.2, meaning Android 17.
    // In that case we get an `EINVAL`.
    //
    // In either of these cases, just fall back to an unsafe `memcpy`.
    if (!rv && (errno == EPERM || errno == EINVAL)) {
        memcpy(dst, src, size);
        rv = true;
    }
    return rv;
}

/**
 * Reads `size` bytes into `dst`, from the `start_offset` inside the `module`.
 */
static bool
sentry__module_read_safely(void *dst, const sentry_module_t *module,
    uint64_t start_offset, uint64_t size)
{
    void *src = sentry__module_get_addr(module, start_offset, size);
    if (!src) {
        return false;
    }
    if (module->is_mmapped) {
        memcpy(dst, src, (size_t)size);
        return true;
    }
    return read_safely(dst, src, (size_t)size);
}

static void
sentry__module_mapping_push(
    sentry_module_t *module, const sentry_parsed_module_t *parsed)
{
    if (module->num_mappings && module->mappings_inode != parsed->inode) {
        return;
    }

    size_t size = parsed->end - parsed->start;
    if (module->num_mappings) {
        sentry_mapped_region_t *last_mapping
            = &module->mappings[module->num_mappings - 1];
        if (last_mapping->addr + last_mapping->size == parsed->start
            && last_mapping->offset + last_mapping->size == parsed->offset) {
            last_mapping->size += size;
            return;
        }
    }
    if (module->num_mappings < SENTRY_MAX_MAPPINGS) {
        sentry_mapped_region_t *mapping
            = &module->mappings[module->num_mappings];
        module->num_mappings += 1;
        mapping->offset = parsed->offset;
        mapping->size = size;
        mapping->addr = parsed->start;

        if (module->num_mappings == 1) {
            module->mappings_inode = parsed->inode;
            module->offset_in_inode = parsed->offset;
        }
    }
}

static bool
is_duplicated_mapping(
    const sentry_module_t *module, const sentry_parsed_module_t *parsed)
{
    if (!module->num_mappings) {
        return false;
    }
    const sentry_mapped_region_t *mapping = &module->mappings[0];
    return (mapping->offset == parsed->offset
        && module->mappings_inode == parsed->inode);
}

int
sentry__procmaps_parse_module_line(
    const char *line, sentry_parsed_module_t *module)
{
    uint8_t major_device;
    uint8_t minor_device;
    int consumed = 0;

    // this has been copied from the breakpad source:
    // https://github.com/google/breakpad/blob/13c1568702e8804bc3ebcfbb435a2786a3e335cf/src/processor/proc_maps_linux.cc#L66
    if (sscanf(line,
            "%" SCNx64 "-%" SCNx64 " %4c %" SCNx64 " %hhx:%hhx %" SCNu64 " %n",
            &module->start, &module->end, &module->permissions[0],
            &module->offset, &major_device, &minor_device, &module->inode,
            &consumed)
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
        // The note header size is independent of the architecture, so we just
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

static const uint8_t *
get_code_id_from_program_header(const sentry_module_t *module, size_t *size_out)
{
    *size_out = 0;

    // iterate over all the program headers, for 32/64 bit separately
    unsigned char e_ident[EI_NIDENT];
    ENSURE(sentry__module_read_safely(e_ident, module, 0, EI_NIDENT));
    if (e_ident[EI_CLASS] == ELFCLASS64) {
        Elf64_Ehdr elf;
        ENSURE(sentry__module_read_safely(&elf, module, 0, sizeof(Elf64_Ehdr)));

        for (uint32_t i = 0; i < elf.e_phnum; i++) {
            Elf64_Phdr header;
            ENSURE(sentry__module_read_safely(&header, module,
                elf.e_phoff + (uint64_t)elf.e_phentsize * i,
                sizeof(Elf64_Phdr)));

            // we are only interested in notes
            if (header.p_type != PT_NOTE) {
                continue;
            }
            void *segment_addr = sentry__module_get_addr(
                module, header.p_offset, header.p_filesz);
            ENSURE(segment_addr);
            const uint8_t *code_id = get_code_id_from_notes(header.p_align,
                segment_addr,
                (void *)((uintptr_t)segment_addr + header.p_filesz), size_out);
            if (code_id) {
                return code_id;
            }
        }
    } else {
        Elf32_Ehdr elf;
        ENSURE(sentry__module_read_safely(&elf, module, 0, sizeof(Elf32_Ehdr)));

        for (uint32_t i = 0; i < elf.e_phnum; i++) {
            Elf32_Phdr header;
            ENSURE(sentry__module_read_safely(&header, module,
                elf.e_phoff + (uint64_t)elf.e_phentsize * i,
                sizeof(Elf32_Phdr)));

            // we are only interested in notes
            if (header.p_type != PT_NOTE) {
                continue;
            }
            void *segment_addr = sentry__module_get_addr(
                module, header.p_offset, header.p_filesz);
            ENSURE(segment_addr);
            const uint8_t *code_id = get_code_id_from_notes(header.p_align,
                segment_addr,
                (void *)((uintptr_t)segment_addr + header.p_filesz), size_out);
            if (code_id) {
                return code_id;
            }
        }
    }
fail:
    return NULL;
}

#define ELF_SECTION_ITER(INNER)                                                \
    unsigned char e_ident[EI_NIDENT];                                          \
    ENSURE(sentry__module_read_safely(e_ident, module, 0, EI_NIDENT));         \
    if (e_ident[EI_CLASS] == ELFCLASS64) {                                     \
        Elf64_Ehdr elf;                                                        \
        ENSURE(                                                                \
            sentry__module_read_safely(&elf, module, 0, sizeof(Elf64_Ehdr)));  \
                                                                               \
        Elf64_Shdr strheader;                                                  \
        ENSURE(sentry__module_read_safely(&strheader, module,                  \
            elf.e_shoff + (uint64_t)elf.e_shentsize * elf.e_shstrndx,          \
            sizeof(Elf64_Shdr)));                                              \
                                                                               \
        for (uint32_t i = 0; i < elf.e_shnum; i++) {                           \
            Elf64_Shdr header;                                                 \
            ENSURE(sentry__module_read_safely(&header, module,                 \
                elf.e_shoff + (uint64_t)elf.e_shentsize * i,                   \
                sizeof(Elf64_Shdr)));                                          \
                                                                               \
            char name[6];                                                      \
            ENSURE(sentry__module_read_safely(name, module,                    \
                strheader.sh_offset + header.sh_name, sizeof(name)));          \
            name[5] = '\0';                                                    \
                                                                               \
            INNER                                                              \
        }                                                                      \
    } else {                                                                   \
        Elf32_Ehdr elf;                                                        \
        ENSURE(                                                                \
            sentry__module_read_safely(&elf, module, 0, sizeof(Elf32_Ehdr)));  \
                                                                               \
        Elf32_Shdr strheader;                                                  \
        ENSURE(sentry__module_read_safely(&strheader, module,                  \
            elf.e_shoff + (uint64_t)elf.e_shentsize * elf.e_shstrndx,          \
            sizeof(Elf32_Shdr)));                                              \
                                                                               \
        for (uint32_t i = 0; i < elf.e_shnum; i++) {                           \
            Elf32_Shdr header;                                                 \
            ENSURE(sentry__module_read_safely(&header, module,                 \
                elf.e_shoff + (uint64_t)elf.e_shentsize * i,                   \
                sizeof(Elf32_Shdr)));                                          \
                                                                               \
            char name[6];                                                      \
            ENSURE(sentry__module_read_safely(name, module,                    \
                strheader.sh_offset + header.sh_name, sizeof(name)));          \
            name[5] = '\0';                                                    \
                                                                               \
            INNER                                                              \
        }                                                                      \
    }

static const uint8_t *
get_code_id_from_note_section(const sentry_module_t *module, size_t *size_out)
{
    *size_out = 0;

    ELF_SECTION_ITER(
        if (header.sh_type == SHT_NOTE && strcmp(name, ".note") == 0) {
            void *segment_addr = sentry__module_get_addr(
                module, header.sh_offset, header.sh_size);
            ENSURE(segment_addr);
            const uint8_t *code_id = get_code_id_from_notes(header.sh_addralign,
                segment_addr,
                (void *)((uintptr_t)segment_addr + header.sh_size), size_out);
            if (code_id) {
                return code_id;
            }
        })
fail:
    return NULL;
}

static sentry_uuid_t
get_code_id_from_text_section(const sentry_module_t *module)
{
    const uint8_t *text = NULL;
    size_t text_size = 0;

    ELF_SECTION_ITER(
        if (header.sh_type == SHT_PROGBITS && strcmp(name, ".text") == 0) {
            text = sentry__module_get_addr(
                module, header.sh_offset, header.sh_size);
            ENSURE(text);
            text_size = header.sh_size;
            break;
        })

    sentry_uuid_t uuid = sentry_uuid_nil();

    // adapted from
    // https://github.com/getsentry/symbolic/blob/8f9a01756e48dcbba2e42917a064f495d74058b7/debuginfo/src/elf.rs#L100-L110
    size_t max = MIN(text_size, 4096);
    for (size_t i = 0; i < max; i++) {
        uuid.bytes[i % 16] ^= text[i];
    }

    return uuid;
fail:
    return sentry_uuid_nil();
}

#undef ELF_SECTION_ITER

bool
sentry__procmaps_read_ids_from_elf(
    sentry_value_t value, const sentry_module_t *module)
{
    // try to get the debug id from the elf headers of the loaded modules
    size_t code_id_size;
    const uint8_t *code_id
        = get_code_id_from_program_header(module, &code_id_size);
    sentry_uuid_t uuid = sentry_uuid_nil();

    if (code_id) {
        sentry_value_set_by_key(value, "code_id",
            sentry__value_new_hexstring(code_id, code_id_size));

        memcpy(uuid.bytes, code_id, MIN(code_id_size, 16));
    } else {
        // no code-id found, try the ".note.gnu.build-id" section
        code_id = get_code_id_from_note_section(module, &code_id_size);
        if (code_id) {
            sentry_value_set_by_key(value, "code_id",
                sentry__value_new_hexstring(code_id, code_id_size));

            memcpy(uuid.bytes, code_id, MIN(code_id_size, 16));
        } else {
            // We were not able to locate the code-id, so fall back to
            // hashing the first page of the ".text" (program code)
            // section.
            uuid = get_code_id_from_text_section(module);
        }
    }

    // the usage of these is described here:
    // https://getsentry.github.io/symbolicator/advanced/symbol-server-compatibility/#identifiers
    // in particular, the debug_id is a `little-endian GUID`, so we have
    // to do appropriate byte-flipping
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
    sentry_value_set_by_key(mod_val, "code_file",
        sentry__value_new_string_owned(sentry__slice_to_owned(module->file)));

    const sentry_mapped_region_t *first_mapping = &module->mappings[0];
    const sentry_mapped_region_t *last_mapping
        = &module->mappings[module->num_mappings - 1];
    uint64_t module_size
        = last_mapping->addr + last_mapping->size - first_mapping->addr;

    sentry_value_set_by_key(
        mod_val, "image_addr", sentry__value_new_addr(first_mapping->addr));
    sentry_value_set_by_key(
        mod_val, "image_size", sentry_value_new_int32(module_size));

    // At least on the android API-16, x86 simulator, the linker
    // apparently does not load the complete file into memory. Or at
    // least, the section headers which are located at the end of the
    // file are not loaded, and we would be poking into invalid memory.
    // To be safe, we mmap the complete file from disk, so we have the
    // on-disk layout, and are independent of how the runtime linker
    // would load or re-order any sections. The exception here is the
    // linux-gate, which is not an actual file on disk, so we actually
    // poke at its memory.
    if (sentry__slice_eq(module->file, LINUX_GATE)) {
        sentry__procmaps_read_ids_from_elf(mod_val, module);
    } else {
        char *filename = sentry__slice_to_owned(module->file);
        sentry_mmap_t mm;
        if (!sentry__mmap_file(&mm, filename)) {
            sentry_free(filename);
            sentry_value_decref(mod_val);
            return sentry_value_new_null();
        }
        sentry_free(filename);

        sentry_module_t mmapped_module;
        memset(&mmapped_module, 0, sizeof(sentry_module_t));
        mmapped_module.is_mmapped = true;
        mmapped_module.num_mappings = 1;
        mmapped_module.mappings[0].addr
            = (uint64_t)mm.ptr + module->offset_in_inode;
        mmapped_module.mappings[0].size = mm.len - module->offset_in_inode;

        sentry__procmaps_read_ids_from_elf(mod_val, &mmapped_module);

        sentry__mmap_close(&mm);
    }

    return mod_val;
}

static void
try_append_module(sentry_value_t modules, const sentry_module_t *module)
{
    if (!module->file.ptr || !module->num_mappings) {
        return;
    }

    sentry_value_t mod_val = sentry__procmaps_module_to_value(module);
    if (!sentry_value_is_null(mod_val)) {
        sentry_value_append(modules, mod_val);
    }
}

// copied from:
// https://github.com/google/breakpad/blob/eb28e7ed9c1c1e1a717fa34ce0178bf471a6311f/src/client/linux/minidump_writer/linux_dumper.h#L61-L69
#if defined(__i386) || defined(__ARM_EABI__)                                   \
    || (defined(__mips__) && _MIPS_SIM == _ABIO32)                             \
    || (defined(__riscv) && __riscv_xlen == 32)
typedef Elf32_auxv_t elf_aux_entry;
#elif defined(__x86_64) || defined(__aarch64__)                                \
    || (defined(__mips__) && _MIPS_SIM != _ABIO32)                             \
    || (defined(__riscv) && __riscv_xlen == 64)
typedef Elf64_auxv_t elf_aux_entry;
#endif

// See http://man7.org/linux/man-pages/man7/vdso.7.html
static uint64_t
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
            return (uint64_t)one_aux_entry.a_un.a_val;
        }
    }
    close(fd);
    return 0;
}

static bool
is_valid_elf_header(void *start)
{
    unsigned char e_ident[EI_NIDENT];
    if (!read_safely(e_ident, start, EI_NIDENT)) {
        return false;
    }
    return e_ident[EI_MAG0] == ELFMAG0 && e_ident[EI_MAG1] == ELFMAG1
        && e_ident[EI_MAG2] == ELFMAG2 && e_ident[EI_MAG3] == ELFMAG3;
}

static void
load_modules(sentry_value_t modules)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        return;
    }

    // Read the whole map at once. Doing it line-by-line would be a good
    // followup.
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    while (true) {
        char *buf = sentry__stringbuilder_reserve(&sb, 4096);
        if (!buf) {
            sentry__stringbuilder_cleanup(&sb);
            close(fd);
            return;
        }
        ssize_t n = read(fd, buf, 4096);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        } else if (n <= 0) {
            break;
        }
        sentry__stringbuilder_set_len(&sb, sentry__stringbuilder_len(&sb) + n);
    }
    close(fd);

    // ensure the buffer is zero terminated
    if (sentry__stringbuilder_append(&sb, "")) {
        sentry__stringbuilder_cleanup(&sb);
        return;
    }
    char *contents = sentry__stringbuilder_into_string(&sb);
    if (!contents) {
        return;
    }
    char *current_line = contents;

    uint64_t linux_vdso = get_linux_vdso();

    // we have multiple memory maps per file, and we need to merge their
    // offsets based on the filename. Luckily, the maps are ordered by
    // filename, so yay
    sentry_module_t last_module;
    memset(&last_module, 0, sizeof(sentry_module_t));
    while (true) {
        sentry_parsed_module_t module;
        memset(&module, 0, sizeof(sentry_parsed_module_t));
        int read = sentry__procmaps_parse_module_line(current_line, &module);
        current_line += read;
        if (!read) {
            break;
        }

        // skip mappings that are not readable
        if (!module.start || module.permissions[0] != 'r') {
            continue;
        }
        // skip mappings in `/dev/` or mappings that have no filename
        if (!module.file.len
            || (module.file.len >= 5
                && memcmp("/dev/", module.file.ptr, 5) == 0)) {
            continue;
        }
        // for the vdso, we use the special filename `linux-gate.so`
        if (module.start && module.start == linux_vdso) {
            module.file = LINUX_GATE;
        } else if (module.file.ptr[0] != '/') {
            // and skip all mappings that are not a file
            continue;
        }

        if (is_valid_elf_header((void *)(size_t)module.start)) {
            // clang-format off
            // On android, we sometimes have multiple mappings for the
            // same inode at the same offset, such as this:
            // 737b5570d000-737b5570e000 r--p 00000000 07:70 34 /apex/com.android.runtime/lib64/bionic/libdl.so
            // 737b5570e000-737b5570f000 r-xp 00000000 07:70 34 /apex/com.android.runtime/lib64/bionic/libdl.so
            // 737b5570f000-737b55710000 r--p 00000000 07:70 34 /apex/com.android.runtime/lib64/bionic/libdl.so
            // clang-format on

            if (!is_duplicated_mapping(&last_module, &module)) {
                // try to append the module based on the mappings that
                // we have found so far
                try_append_module(modules, &last_module);

                // start a new module based on the current mapping
                memset(&last_module, 0, sizeof(sentry_module_t));

                last_module.file = module.file;
            }
        }

        sentry__module_mapping_push(&last_module, &module);
    }
    try_append_module(modules, &last_module);
    sentry_free(contents);
}

sentry_value_t
sentry_get_modules_list(void)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(g_mutex);
    sentry__mutex_lock(&g_mutex);
    if (!g_initialized) {
        g_modules = sentry_value_new_list();
        SENTRY_DEBUG("trying to read modules from /proc/self/maps");
        load_modules(g_modules);
        SENTRY_DEBUGF("read %zu modules from /proc/self/maps",
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
    SENTRY__MUTEX_INIT_DYN_ONCE(g_mutex);
    sentry__mutex_lock(&g_mutex);
    sentry_value_decref(g_modules);
    g_modules = sentry_value_new_null();
    g_initialized = false;
    sentry__mutex_unlock(&g_mutex);
}
