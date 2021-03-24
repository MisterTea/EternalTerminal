#ifndef SENTRY_PROCMAPS_MODULEFINDER_H_INCLUDED
#define SENTRY_PROCMAPS_MODULEFINDER_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_slice.h"

typedef struct {
    void *start;
    void *end;
    sentry_slice_t file;
} sentry_module_t;

typedef struct {
    void *ptr;
    size_t len;
    int fd;
} sentry_mmap_t;

#if SENTRY_UNITTEST
bool sentry__mmap_file(sentry_mmap_t *mapping, const char *path);
void sentry__mmap_close(sentry_mmap_t *mapping);

bool sentry__procmaps_read_ids_from_elf(sentry_value_t value, void *elf_ptr);

int sentry__procmaps_parse_module_line(
    const char *line, sentry_module_t *module);
#endif

#endif
