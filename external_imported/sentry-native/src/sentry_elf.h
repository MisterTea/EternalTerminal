#ifndef SENTRY_ELF_H_INCLUDED
#define SENTRY_ELF_H_INCLUDED

#include "sentry_boot.h"

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)

#    include <elf.h>
#    include <stdbool.h>
#    include <stddef.h>
#    include <string.h>

static inline bool
sentry__elf_is_native_class(const unsigned char e_ident[EI_NIDENT])
{
#    if defined(__x86_64__) || defined(__aarch64__)
    return e_ident[EI_CLASS] == ELFCLASS64;
#    else
    return e_ident[EI_CLASS] == ELFCLASS32;
#    endif
}

static inline bool
sentry__elf_has_shdr_size(
    const unsigned char e_ident[EI_NIDENT], size_t e_shentsize)
{
    if (!sentry__elf_is_native_class(e_ident)) {
        return false;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    return e_shentsize == sizeof(Elf64_Shdr);
#    else
    return e_shentsize == sizeof(Elf32_Shdr);
#    endif
}

static inline bool
sentry__elf_has_sym_entsize(
    const unsigned char e_ident[EI_NIDENT], size_t sh_entsize)
{
    if (!sentry__elf_is_native_class(e_ident)) {
        return false;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    return sh_entsize == sizeof(Elf64_Sym);
#    else
    return sh_entsize == sizeof(Elf32_Sym);
#    endif
}

static inline bool
sentry__elf_has_phdr_size(
    const unsigned char e_ident[EI_NIDENT], size_t e_phentsize)
{
    if (!sentry__elf_is_native_class(e_ident)) {
        return false;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    return e_phentsize == sizeof(Elf64_Phdr);
#    else
    return e_phentsize == sizeof(Elf32_Phdr);
#    endif
}

/**
 * Safely iterate ELF notes in a buffer to find one matching the given type
 * and name. Alignment must be 4 or 8 (per ELF spec, PT_NOTE uses p_align,
 * SHT_NOTE uses 4). Returns a pointer to the descriptor data on success, or
 * NULL if the note is not found or the buffer is malformed.
 */
static inline const uint8_t *
sentry__elf_find_note(const void *buf, size_t buf_size, size_t alignment,
    uint32_t type, const char *name, size_t name_len, size_t *desc_size_out)
{
    if (!buf || !desc_size_out) {
        return NULL;
    }

    if (alignment < 4) {
        alignment = 4;
    } else if (alignment != 4 && alignment != 8) {
        return NULL;
    }

    const uint8_t *start = (const uint8_t *)buf;
    size_t offset = 0;

    while (buf_size - offset >= sizeof(Elf64_Nhdr)) {
        const Elf64_Nhdr *note = (const Elf64_Nhdr *)(start + offset);
        size_t name_size = note->n_namesz;
        size_t desc_size = note->n_descsz;
        size_t mask = alignment - 1;

        if (name_size > SIZE_MAX - mask || desc_size > SIZE_MAX - mask) {
            return NULL;
        }

        offset += sizeof(Elf64_Nhdr);

        if (name_size > buf_size - offset) {
            return NULL;
        }

        size_t name_end = offset + name_size;
        size_t desc_start = (name_end + mask) & ~mask;

        if (desc_start > buf_size) {
            return NULL;
        }

        if (desc_size > buf_size - desc_start) {
            return NULL;
        }

        const uint8_t *desc = start + desc_start;

        if (note->n_type == type && name_size == name_len
            && memcmp(start + offset, name, name_len) == 0) {
            *desc_size_out = desc_size;
            return desc;
        }

        offset = (desc_start + desc_size + mask) & ~mask;
        if (offset > buf_size) {
            break;
        }
    }

    return NULL;
}

#endif

#endif
