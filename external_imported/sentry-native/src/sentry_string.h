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
 * Appends a zero terminated string to the builder.
 */
int sentry__stringbuilder_append(sentry_stringbuilder_t *sb, const char *s);

/**
 * Appends a sized buffer.
 */
int sentry__stringbuilder_append_buf(
    sentry_stringbuilder_t *sb, const char *s, size_t len);

/**
 * Appends a single character.
 */
int sentry__stringbuilder_append_char(sentry_stringbuilder_t *sb, char c);

/**
 * Appends a utf-32 character.
 */
int sentry__stringbuilder_append_char32(sentry_stringbuilder_t *sb, uint32_t c);

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
 * Duplicates a zero terminated string.
 */
char *sentry__string_clone(const char *str);

/**
 * Duplicates a zero terminated string with a length limit.
 */
char *sentry__string_clonen(const char *str, size_t n);

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
