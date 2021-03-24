#ifndef SENTRY_SLICE_H_INCLUDED
#define SENTRY_SLICE_H_INCLUDED

#include "sentry_boot.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * This represents an explicit non-zero-terminated string slice.
 * The slice only refers to borrowed memory.
 */
typedef struct {
    const char *ptr;
    size_t len;
} sentry_slice_t;

/**
 * Create a slice from a zero-terminated string.
 */
sentry_slice_t sentry__slice_from_str(const char *str);

/**
 * Creates an owned copy from a slice.
 */
char *sentry__slice_to_owned(sentry_slice_t slice);

/**
 * Compares two slices
 */
bool sentry__slice_eq(sentry_slice_t a, sentry_slice_t b);

/**
 * Compares a slice and a zero-terminated string
 */
static inline bool
sentry__slice_eqs(sentry_slice_t a, const char *str)
{
    return sentry__slice_eq(a, sentry__slice_from_str(str));
}

/**
 * Returns the left sub-slice, up to `c`, or the complete slice if `c` was not
 * found.
 */
sentry_slice_t sentry__slice_split_at(sentry_slice_t a, char c);

/**
 * Returns the index of `c` inside of the slice `a`, or `(size_t)-1` if `c` was
 * not found.
 */
size_t sentry__slice_find(sentry_slice_t a, char c);

/**
 * This trims off leading and trailing whitespace.
 */
sentry_slice_t sentry__slice_trim(sentry_slice_t a);

/**
 * When the given slice starts with `c`, it will skip over the character, or
 * otherwise return `false`.
 */
static inline bool
sentry__slice_consume_if(sentry_slice_t *a, char c)
{
    if (a->len > 0 && a->ptr[0] == c) {
        a->ptr++;
        a->len--;
        return true;
    } else {
        return false;
    }
}

/**
 * This will consume a leading uint64 number from the slice, and write it into
 * `num_out`, or otherwise return `false`.
 */
bool sentry__slice_consume_uint64(sentry_slice_t *a, uint64_t *num_out);

/**
 * This moves the slice ahead by `bytes`.
 */
static inline sentry_slice_t
sentry__slice_advance(sentry_slice_t s, size_t bytes)
{
    s.ptr += bytes;
    s.len -= bytes;
    return s;
}

#endif
