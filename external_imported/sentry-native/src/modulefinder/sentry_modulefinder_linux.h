#ifndef SENTRY_PROCMAPS_MODULEFINDER_H_INCLUDED
#define SENTRY_PROCMAPS_MODULEFINDER_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_slice.h"

#define SENTRY_MAX_MAPPINGS 5

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t offset;
    char permissions[5];
    uint64_t inode;
    sentry_slice_t file;
} sentry_parsed_module_t;

typedef struct {
    uint64_t offset; // the offset in the mapped file
    uint64_t size; // size of this mapping
    uint64_t addr; // addr in memory of the mapping
} sentry_mapped_region_t;

typedef struct {
    sentry_slice_t file;
    sentry_mapped_region_t mappings[SENTRY_MAX_MAPPINGS];
    uint64_t offset_in_inode;
    uint64_t mappings_inode;
    uint8_t num_mappings;
    bool is_mmapped;
} sentry_module_t;

typedef struct {
    void *ptr;
    size_t len;
} sentry_mmap_t;

#ifdef SENTRY_UNITTEST
bool sentry__procmaps_read_ids_from_elf(
    sentry_value_t value, const sentry_module_t *module);

int sentry__procmaps_parse_module_line(
    const char *line, sentry_parsed_module_t *module);

/**
 * Checks that `start_offset` + `size` is a valid contiguous mapping in the
 * mapped regions, and returns the translated pointer corresponding to
 * `start_offset`.
 */
void *sentry__module_get_addr(
    const sentry_module_t *module, uint64_t start_offset, uint64_t size);
#endif

#endif
