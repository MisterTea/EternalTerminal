#ifndef SENTRY_MINIDUMP_INDIRECT_H_INCLUDED
#define SENTRY_MINIDUMP_INDIRECT_H_INCLUDED

#include "sentry_minidump_common.h"
#include "sentry_minidump_format.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * Indirectly-referenced memory capture for SMART minidump mode.
 *
 * Mirrors the Windows MiniDumpWithIndirectlyReferencedMemory contract:
 * for every captured thread, scan its registers and stack words, and for
 * each value that resolves into a writable heap region capture a small
 * page-aligned chunk of memory around it. Lets debuggers (LLDB, VS Code)
 * deref pointers held in struct locals or registers at crash time.
 *
 * This file is a small algorithm + a 2-function vtable. Each platform
 * (linux, macos) provides the vtable and feeds in the per-thread
 * registers and stack bytes; the dedup, paging, and dump-emission logic
 * lives here once.
 */

// Per-pointer capture size — matches the Windows API default of 1 KiB.
#define SENTRY_INDIRECT_PER_POINTER_BYTES 1024
// Hard caps so dump size doesn't blow up under pathological pointer density.
#define SENTRY_INDIRECT_MAX_REGIONS 1024
#define SENTRY_INDIRECT_MAX_TOTAL_BYTES (4 * 1024 * 1024)
// Page size used for region alignment. 4 KiB on every arch we target.
#define SENTRY_INDIRECT_PAGE_SIZE 4096

typedef struct {
    uint64_t start; // page-aligned target VA
    uint64_t end; // exclusive
    minidump_rva_t rva;
    uint32_t size; // bytes actually present in the dump
} sentry_indirect_region_t;

/**
 * Platform shim. Each backend supplies its own pointer-validation and
 * remote-memory-read implementations; the algorithm calls these via this
 * tiny vtable instead of duplicating the loop in every platform file.
 */
typedef struct sentry_indirect_ops_s {
    /**
     * Returns true iff `addr` falls inside a readable, writable, non-executable
     * mapping that is NOT a thread stack, vDSO, or other kernel-private region
     * — i.e. the kind of mapping a heap pointer would target.
     *
     * Called O(words-on-stack) times per dump, so should be O(log n) over the
     * cached mappings table (binary search) rather than a linear scan.
     */
    bool (*is_writable_heap)(void *ctx, uint64_t addr);

    /**
     * Read up to `len` bytes from the *target* (crashed) process at virtual
     * address `addr` into `buf`. Returns bytes read on success, or -1 on
     * failure.
     */
    ssize_t (*read_memory)(void *ctx, uint64_t addr, void *buf, size_t len);

    /** Opaque pointer passed to the callbacks above. */
    void *ctx;
} sentry_indirect_ops_t;

/**
 * Bounded accumulator. Holds the regions captured so far plus the running
 * byte total used to enforce SENTRY_INDIRECT_MAX_TOTAL_BYTES.
 *
 * Regions are kept sorted by `start` so dedup is O(log n) per candidate.
 */
typedef struct {
    sentry_indirect_region_t regions[SENTRY_INDIRECT_MAX_REGIONS];
    size_t region_count;
    size_t total_bytes;
} sentry_indirect_accumulator_t;

void sentry__indirect_init(sentry_indirect_accumulator_t *acc);

/**
 * Consider one address as a candidate heap pointer. If `addr` resolves into
 * a writable-heap mapping (per ops->is_writable_heap), is not already covered
 * by a previously-captured region, and the global byte budget hasn't been
 * exhausted, this captures SENTRY_INDIRECT_PER_POINTER_BYTES around `addr`,
 * page-aligned, and writes them to the dump file via `writer`.
 *
 * Safe to call with junk values — non-pointer addresses are filtered cheaply
 * via the writable-heap check.
 */
void sentry__indirect_consider(sentry_indirect_accumulator_t *acc,
    minidump_writer_base_t *writer, uint64_t addr,
    const sentry_indirect_ops_t *ops);

/**
 * Walk a buffer of pointer-sized words and call sentry__indirect_consider on
 * each. `buf` is treated as an array of native-endian uint64_t (or uint32_t
 * on 32-bit; the function handles both based on sizeof(void*)). `len_bytes`
 * may be unaligned — the trailing bytes shorter than a word are ignored.
 *
 * This is the hot loop for stack scanning: a 1 MiB stack on AArch64 yields
 * 128 K candidates, each costing one O(log n) mapping lookup.
 */
void sentry__indirect_walk_words(sentry_indirect_accumulator_t *acc,
    minidump_writer_base_t *writer, const void *buf, size_t len_bytes,
    const sentry_indirect_ops_t *ops);

#endif // SENTRY_MINIDUMP_INDIRECT_H_INCLUDED
