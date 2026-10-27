#ifndef SENTRY_MINIDUMP_COMMON_H_INCLUDED
#define SENTRY_MINIDUMP_COMMON_H_INCLUDED

#include "sentry_minidump_format.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Common minidump writer base structure
 * Platform-specific writers embed this as their first member
 */
typedef struct {
    int fd;
    uint64_t current_offset;
} minidump_writer_base_t;

/**
 * Write data to minidump file and return RVA
 * Automatically aligns to 4-byte boundary
 *
 * @param writer Pointer to writer base (or struct with base as first member)
 * @param data Data to write
 * @param size Size of data
 * @return RVA of written data, or 0 on failure
 */
minidump_rva_t sentry__minidump_write_data(
    minidump_writer_base_t *writer, const void *data, size_t size);

/**
 * Write minidump header
 *
 * @param writer Pointer to writer base
 * @param stream_count Number of streams in the minidump
 * @return 0 on success, -1 on failure
 */
int sentry__minidump_write_header(
    minidump_writer_base_t *writer, uint32_t stream_count);

/**
 * Write UTF-16LE string for minidump
 * Converts UTF-8 string to UTF-16LE with length prefix
 *
 * @param writer Pointer to writer base
 * @param utf8_str UTF-8 string to convert and write
 * @return RVA of written string, or 0 on failure
 */
minidump_rva_t sentry__minidump_write_string(
    minidump_writer_base_t *writer, const char *utf8_str);

/**
 * Get size of thread context for current architecture
 *
 * @return Size of the architecture-specific context structure
 */
size_t sentry__minidump_get_context_size(void);

#endif // SENTRY_MINIDUMP_COMMON_H_INCLUDED
