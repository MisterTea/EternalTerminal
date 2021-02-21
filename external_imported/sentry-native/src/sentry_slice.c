#include "sentry_slice.h"
#include "sentry_string.h"
#include <stdlib.h>
#include <string.h>

sentry_slice_t
sentry__slice_from_str(const char *str)
{
    sentry_slice_t rv;
    rv.ptr = str;
    rv.len = str ? strlen(str) : 0;
    return rv;
}

char *
sentry__slice_to_owned(sentry_slice_t slice)
{
    return sentry__string_clonen(slice.ptr, slice.len);
}

bool
sentry__slice_eq(sentry_slice_t a, sentry_slice_t b)
{
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

sentry_slice_t
sentry__slice_split_at(sentry_slice_t a, char c)
{
    for (size_t i = 0; i < a.len; i++) {
        if (a.ptr[i] == c) {
            a.len = i;
            return a;
        }
    }
    return a;
}

size_t
sentry__slice_find(sentry_slice_t a, char c)
{
    for (size_t i = 0; i < a.len; i++) {
        if (a.ptr[i] == c) {
            return i;
        }
    }
    return (size_t)-1;
}

static bool
is_space(char c)
{
    switch (c) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
        return true;
    default:
        return false;
    }
}

sentry_slice_t
sentry__slice_trim(sentry_slice_t a)
{
    while (a.len > 0 && is_space(a.ptr[0])) {
        a.ptr++;
        a.len--;
    }
    while (a.len > 0 && is_space(a.ptr[a.len - 1])) {
        a.len--;
    }

    return a;
}

bool
sentry__slice_consume_uint64(sentry_slice_t *a, uint64_t *num_out)
{
    bool rv = false;
    char *buf = sentry_malloc(a->len + 1);
    memcpy(buf, a->ptr, a->len);
    buf[a->len] = 0;
    char *end;
    *num_out = (uint64_t)strtoll(buf, &end, 10);
    if (end != buf) {
        size_t diff = (uintptr_t)end - (uintptr_t)buf;
        a->len -= diff;
        a->ptr += diff;
        rv = true;
    }
    sentry_free(buf);
    return rv;
}
