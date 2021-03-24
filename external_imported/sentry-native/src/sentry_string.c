#include <string.h>

#include "sentry_alloc.h"
#include "sentry_string.h"

#define INITIAL_BUFFER_SIZE 128

void
sentry__stringbuilder_init(sentry_stringbuilder_t *sb)
{
    sb->buf = NULL;
    sb->allocated = 0;
    sb->len = 0;
}

static int
append(sentry_stringbuilder_t *sb, const char *s, size_t len)
{
    size_t needed = sb->len + len + 1;
    if (!sb->buf || needed > sb->allocated) {
        size_t new_alloc_size = sb->allocated;
        if (new_alloc_size == 0) {
            new_alloc_size = INITIAL_BUFFER_SIZE;
        }
        while (new_alloc_size < needed) {
            new_alloc_size = new_alloc_size * 2;
        }
        char *new_buf = sentry_malloc(new_alloc_size);
        if (!new_buf) {
            return 1;
        }
        if (sb->buf) {
            memcpy(new_buf, sb->buf, sb->allocated);
            sentry_free(sb->buf);
        }
        sb->buf = new_buf;
        sb->allocated = new_alloc_size;
    }
    memcpy(sb->buf + sb->len, s, len);
    sb->len += len;

    // make sure we're always zero terminated
    sb->buf[sb->len] = '\0';

    return 0;
}

int
sentry__stringbuilder_append(sentry_stringbuilder_t *sb, const char *s)
{
    return append(sb, s, strlen(s));
}

int
sentry__stringbuilder_append_buf(
    sentry_stringbuilder_t *sb, const char *s, size_t len)
{
    return append(sb, s, len);
}

int
sentry__stringbuilder_append_char(sentry_stringbuilder_t *sb, char c)
{
    return append(sb, &c, 1);
}

int
sentry__stringbuilder_append_char32(sentry_stringbuilder_t *sb, uint32_t c)
{
    char buf[4];
    size_t len = sentry__unichar_to_utf8(c, buf);
    return sentry__stringbuilder_append_buf(sb, buf, len);
}

char *
sentry_stringbuilder_take_string(sentry_stringbuilder_t *sb)
{
    char *rv = sb->buf;
    if (!rv) {
        rv = sentry__string_clone("");
    }
    sb->buf = NULL;
    sb->allocated = 0;
    sb->len = 0;
    return rv;
}

char *
sentry__stringbuilder_into_string(sentry_stringbuilder_t *sb)
{
    char *rv = sentry_stringbuilder_take_string(sb);
    sentry__stringbuilder_cleanup(sb);
    return rv;
}

void
sentry__stringbuilder_cleanup(sentry_stringbuilder_t *sb)
{
    sentry_free(sb->buf);
}

size_t
sentry__stringbuilder_len(const sentry_stringbuilder_t *sb)
{
    return sb->len;
}

char *
sentry__string_clone(const char *str)
{
    return str ? sentry__string_clonen(str, strlen(str)) : NULL;
}

char *
sentry__string_clonen(const char *str, size_t n)
{
    size_t len = n + 1;
    char *rv = sentry_malloc(len);
    if (rv) {
        memcpy(rv, str, n);
        rv[n] = 0;
    }
    return rv;
}

#ifdef SENTRY_PLATFORM_WINDOWS
char *
sentry__string_from_wstr(const wchar_t *s)
{
    if (!s) {
        return NULL;
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *rv = sentry_malloc(len);
    if (rv) {
        WideCharToMultiByte(CP_UTF8, 0, s, -1, rv, len, NULL, NULL);
    }
    return rv;
}

wchar_t *
sentry__string_to_wstr(const char *s)
{
    size_t len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *rv = sentry_malloc(sizeof(wchar_t) * len);
    if (rv) {
        MultiByteToWideChar(CP_UTF8, 0, s, -1, rv, (int)len);
    }
    return rv;
}
#endif

size_t
sentry__unichar_to_utf8(uint32_t c, char *buf)
{
    size_t i, len = 1;
    uint32_t first;

    if (c < 0x80) {
        first = 0;
        len = 1;
    } else if (c < 0x800) {
        first = 0xc0;
        len = 2;
    } else if (c < 0x10000) {
        first = 0xe0;
        len = 3;
    } else if (c <= 0x10FFFF) {
        first = 0xf0;
        len = 4;
    } else {
        return 0;
    }

    for (i = len - 1; i > 0; --i) {
        buf[i] = (char)(c & 0x3f) | 0x80;
        c >>= 6;
    }
    buf[0] = (char)(c | first);
    return len;
}
