#include "sentry_boot.h"

#include "sentry_minidump_indirect.h"

#include "sentry_alloc.h"
#include "sentry_logger.h"
#include "sentry_minidump_common.h"

#include <stdint.h>
#include <string.h>

void
sentry__indirect_init(sentry_indirect_accumulator_t *acc)
{
    acc->region_count = 0;
    acc->total_bytes = 0;
}

/**
 * Binary-search the sorted region list for the first entry whose `end` is
 * strictly greater than `target`. The candidate at index `lo` (if any)
 * is the only one that can possibly overlap [target, target+len).
 *
 * Returns region_count when no such entry exists (target is past the last).
 */
static size_t
find_first_after(const sentry_indirect_accumulator_t *acc, uint64_t target)
{
    size_t lo = 0;
    size_t hi = acc->region_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (acc->regions[mid].end <= target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/**
 * Returns true if [start, end) overlaps any region already in the
 * accumulator. The list is sorted, so we only need to check the first
 * region whose end > start — anything earlier ended before us, anything
 * later starts after we'd want it to.
 */
static bool
overlaps_existing(
    const sentry_indirect_accumulator_t *acc, uint64_t start, uint64_t end)
{
    size_t i = find_first_after(acc, start);
    if (i >= acc->region_count) {
        return false;
    }
    return acc->regions[i].start < end;
}

/**
 * Insert a new region at the sorted position. Caller must have verified
 * via overlaps_existing() that no overlap exists, and via the cap checks
 * that there's room.
 */
static void
insert_sorted(sentry_indirect_accumulator_t *acc,
    const sentry_indirect_region_t *new_region)
{
    size_t i = find_first_after(acc, new_region->start);
    if (i < acc->region_count) {
        memmove(&acc->regions[i + 1], &acc->regions[i],
            (acc->region_count - i) * sizeof(*acc->regions));
    }
    acc->regions[i] = *new_region;
    acc->region_count++;
    acc->total_bytes += new_region->size;
}

void
sentry__indirect_consider(sentry_indirect_accumulator_t *acc,
    minidump_writer_base_t *writer, uint64_t addr,
    const sentry_indirect_ops_t *ops)
{
    // Cap checks first — they're the cheapest filters and short-circuit the
    // mapping lookup once we've spent the budget.
    if (acc->region_count >= SENTRY_INDIRECT_MAX_REGIONS) {
        return;
    }
    if (acc->total_bytes >= SENTRY_INDIRECT_MAX_TOTAL_BYTES) {
        return;
    }

    // Cheap rejects before paying for is_writable_heap.
    // - 0/low values: NULLs and small ints
    // - very high bits set: kernel addresses (canonical AArch64/x86_64 user
    //   space tops out below this)
    if (addr < SENTRY_INDIRECT_PAGE_SIZE) {
        return;
    }
    if ((addr >> 56) != 0) {
        return;
    }

    if (!ops->is_writable_heap(ops->ctx, addr)) {
        return;
    }

    // Page-align down so the captured region covers the page containing the
    // pointee (and we can dedup multiple pointers landing in the same page).
    // Capture is roughly centered on the pointer but always starts on a page
    // boundary so adjacent allocations get covered too.
    uint64_t centered = addr - (SENTRY_INDIRECT_PER_POINTER_BYTES / 2);
    uint64_t start = centered & ~((uint64_t)SENTRY_INDIRECT_PAGE_SIZE - 1);
    uint64_t end = start + SENTRY_INDIRECT_PER_POINTER_BYTES;
    // Round end up to page boundary too — small bump but keeps reads aligned.
    end = (end + SENTRY_INDIRECT_PAGE_SIZE - 1)
        & ~((uint64_t)SENTRY_INDIRECT_PAGE_SIZE - 1);

    // Trim against the global byte budget so a candidate near the cap doesn't
    // blow it.
    size_t want = (size_t)(end - start);
    size_t remaining = SENTRY_INDIRECT_MAX_TOTAL_BYTES - acc->total_bytes;
    if (want > remaining) {
        end = start + remaining;
        if (end <= start) {
            return;
        }
        want = (size_t)(end - start);
    }

    if (overlaps_existing(acc, start, end)) {
        return;
    }

    // Read from the target. Soft-fail on read errors — a pointer into a
    // mapped-but-paged-out region or a recently-unmapped one is common during
    // crash handling and shouldn't abort the whole walk.
    void *buf = sentry_malloc(want);
    if (!buf) {
        return;
    }
    ssize_t got = ops->read_memory(ops->ctx, start, buf, want);
    if (got <= 0) {
        sentry_free(buf);
        return;
    }

    minidump_rva_t rva = sentry__minidump_write_data(writer, buf, (size_t)got);
    sentry_free(buf);
    if (!rva) {
        return;
    }

    sentry_indirect_region_t r;
    r.start = start;
    r.end = start + (uint64_t)got;
    r.rva = rva;
    r.size = (uint32_t)got;
    insert_sorted(acc, &r);
}

void
sentry__indirect_walk_words(sentry_indirect_accumulator_t *acc,
    minidump_writer_base_t *writer, const void *buf, size_t len_bytes,
    const sentry_indirect_ops_t *ops)
{
    const size_t word_size = sizeof(void *);
    const size_t word_count = len_bytes / word_size;
    if (word_size == 8) {
        const uint64_t *words = (const uint64_t *)buf;
        for (size_t i = 0; i < word_count; i++) {
            sentry__indirect_consider(acc, writer, words[i], ops);
            // Bail early once the byte cap is fully spent — no point grinding
            // through the rest of the stack. The region cap also acts as a
            // floor here.
            if (acc->total_bytes >= SENTRY_INDIRECT_MAX_TOTAL_BYTES
                || acc->region_count >= SENTRY_INDIRECT_MAX_REGIONS) {
                return;
            }
        }
    } else {
        // 32-bit hosts (Linux i386 / arm). Pointers are 32-bit but our
        // accumulator stores them as 64-bit just fine.
        const uint32_t *words = (const uint32_t *)buf;
        for (size_t i = 0; i < word_count; i++) {
            sentry__indirect_consider(acc, writer, (uint64_t)words[i], ops);
            if (acc->total_bytes >= SENTRY_INDIRECT_MAX_TOTAL_BYTES
                || acc->region_count >= SENTRY_INDIRECT_MAX_REGIONS) {
                return;
            }
        }
    }
}
