#ifndef SENTRY_STRING_H_INCLUDED
#define SENTRY_STRING_H_INCLUDED

#include "sentry_boot.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/**
 * A string builder, which can be used as a mutable, growable string buffer.
 */
typedef struct sentry_stringbuilder_s {
    char *buf;
    size_t allocated;
    size_t len;
} sentry_stringbuilder_t;

/**
 * Initializes a new string builder, which is typically allocated on the stack.
 */
void sentry__stringbuilder_init(sentry_stringbuilder_t *sb);

/**
 * Resizes the stringbuilder buffer to make sure there is at least `len` bytes
 * available at the end, and returns a pointer *to the reservation*.
 */
char *sentry__stringbuilder_reserve(sentry_stringbuilder_t *sb, size_t len);

/**
 * Appends a sized buffer.
 */
static inline int
sentry__stringbuilder_append_buf(
    sentry_stringbuilder_t *sb, const char *s, size_t len)
{
    size_t needed = sb->len + len + 1;
    char *buf = sb->buf;
    if (!sb->buf || needed > sb->allocated) {
        buf = sentry__stringbuilder_reserve(sb, len + 1);
        if (!buf) {
            return 1;
        }
    } else {
        buf = buf + sb->len;
    }

    memcpy(buf, s, len);
    sb->len += len;

    // make sure we're always zero terminated
    sb->buf[sb->len] = '\0';

    return 0;
}

/**
 * Appends a zero terminated string to the builder.
 */
static inline int
sentry__stringbuilder_append(sentry_stringbuilder_t *sb, const char *s)
{
    return sentry__stringbuilder_append_buf(sb, s, strlen(s));
}

/**
 * Appends a single character.
 */
static inline int
sentry__stringbuilder_append_char(sentry_stringbuilder_t *sb, char c)
{
    return sentry__stringbuilder_append_buf(sb, &c, 1);
}

/**
 * Appends an int64 value.
 */
static inline int
sentry__stringbuilder_append_int64(sentry_stringbuilder_t *sb, int64_t val)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%" PRId64, val);
    return sentry__stringbuilder_append(sb, buf);
}

/**
 * Detaches the buffer from the string builder and deallocates it.
 */
char *sentry__stringbuilder_into_string(sentry_stringbuilder_t *sb);

/**
 * Detaches the buffer from the string builder.
 */
char *sentry_stringbuilder_take_string(sentry_stringbuilder_t *sb);

/**
 * Deallocates the string builder.
 */
void sentry__stringbuilder_cleanup(sentry_stringbuilder_t *sb);

/**
 * Returns the number of bytes in the string builder.
 */
size_t sentry__stringbuilder_len(const sentry_stringbuilder_t *sb);

/**
 * Sets the number of used bytes in the string builder, to be used together with
 * `sentry__stringbuilder_reserve` to avoid copying from an intermediate buffer.
 */
void sentry__stringbuilder_set_len(sentry_stringbuilder_t *sb, size_t len);

/**
 * Duplicates a zero terminated string with a length limit. Does not check
 * if `str` is NULL.
 */
static inline char *
sentry__string_clone_n_unchecked(const char *str, size_t n)
{
    size_t len = n + 1;
    char *rv = (char *)sentry_malloc(len);
    if (rv) {
        memcpy(rv, str, n);
        rv[n] = 0;
    }
    return rv;
}

/**
 * Duplicates a ptr/len string into a zero terminated string.
 */
static inline char *
sentry__string_clone_n(const char *str, size_t n)
{
    return str ? sentry__string_clone_n_unchecked(str, n) : NULL;
}

/**
 * Duplicates a zero terminated string.
 */
static inline char *
sentry__string_clone(const char *str)
{
    return str ? sentry__string_clone_n_unchecked(str, strlen(str)) : NULL;
}

static inline char *
sentry__string_clone_max_n(const char *str, size_t str_len, size_t max_len)
{
    if (!str) {
        return NULL;
    }
    size_t min_len = str_len < max_len ? str_len : max_len;
    return sentry__string_clone_n_unchecked(str, min_len);
}

/**
 * Converts a string to lowercase.
 */
static inline void
sentry__string_ascii_lower(char *s)
{
    for (; *s; s++) {
        *s = (char)tolower((char)*s);
    }
}

/**
 * Shortcut for string compare.
 */
static inline bool
sentry__string_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/**
 * Converts an int64_t into a string.
 */
static inline char *
sentry__int64_to_string(int64_t val)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%" PRId64, val);
    return sentry__string_clone(buf);
}

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Create a utf-8 string from a Wide String.
 */
char *sentry__string_from_wstr(const wchar_t *s);
/**
 * Convert a normal string to a Wide String.
 */
wchar_t *sentry__string_to_wstr(const char *s);
#endif

/**
 * Writes the utf-8 encoding of unicode character `c` into `buf`, and returns
 * the number of bytes written.
 */
size_t sentry__unichar_to_utf8(uint32_t c, char *buf);

#define sentry__is_lead_surrogate(c) ((c) >= 0xd800 && (c) < 0xdc00)
#define sentry__is_trail_surrogate(c) ((c) >= 0xdc00 && (c) < 0xe000)
#define sentry__surrogate_value(lead, trail)                                   \
    (((((lead)-0xd800) << 10) | ((trail)-0xdc00)) + 0x10000)

#endif
