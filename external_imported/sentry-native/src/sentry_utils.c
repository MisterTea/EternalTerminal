// According to http://lua-users.org/lists/lua-l/2016-04/msg00216.html we can
// use `stdtod_l` on all platforms when defining `_GNU_SOURCE`.

#define _GNU_SOURCE

#include "sentry_utils.h"
#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef SENTRY_PLATFORM_MACOS
#    include <xlocale.h>
#elif defined(SENTRY_PLATFORM_LINUX) && !defined(SENTRY_PLATFORM_ANDROID)
#    include "../vendor/stb_sprintf.h"
#endif

static bool
is_scheme_valid(const char *scheme_name)
{
    char c;
    while ((c = *scheme_name++) != 0) {
        if (!isalpha(c) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

int
sentry__url_parse(sentry_url_t *url_out, const char *url)
{
    bool has_username;
    int result = 0;
    char *scratch = sentry__string_clone(url);
    char *ptr = scratch;
    char *tmp;
    char *aux_buf = 0;
    memset(url_out, 0, sizeof(sentry_url_t));

#define SKIP_WHILE_NOT(ptr, c)                                                 \
    do {                                                                       \
        while (*ptr && *ptr != c)                                              \
            ptr++;                                                             \
    } while (false)
#define SKIP_WHILE_NOT2(ptr, c1, c2)                                           \
    do {                                                                       \
        while (*ptr && *ptr != c1 && *ptr != c2)                               \
            ptr++;                                                             \
    } while (false)

    if (!scratch) {
        goto error;
    }

    /* scheme */
    tmp = strchr(ptr, ':');
    if (!tmp) {
        goto error;
    }
    url_out->scheme = sentry__string_clonen(ptr, tmp - ptr);

    if (!url_out->scheme || !is_scheme_valid(url_out->scheme)) {
        goto error;
    }
    sentry__string_ascii_lower(url_out->scheme);

    ptr = tmp + 1;

    /* scheme trailer */
    if (*ptr++ != '/') {
        goto error;
    }
    if (*ptr++ != '/') {
        goto error;
    }

    // auth
    has_username = false;
    tmp = ptr;
    while (*tmp) {
        if (*tmp == '@') {
            has_username = true;
            break;
        } else if (*tmp == '/') {
            has_username = false;
            break;
        }
        tmp++;
    }
    tmp = ptr;
    if (has_username) {
        SKIP_WHILE_NOT2(tmp, '@', ':');
        url_out->username = sentry__string_clonen(ptr, tmp - ptr);
        ptr = tmp;
        if (*ptr == ':') {
            ptr++;
            tmp = ptr;
            SKIP_WHILE_NOT(tmp, '@');
            url_out->password = sentry__string_clonen(ptr, tmp - ptr);
            ptr = tmp;
        }
        if (*ptr != '@') {
            goto error;
        }
        ptr++;
    }

    /* host */
    bool is_ipv6 = *ptr == '[';
    tmp = ptr;
    while (*tmp) {
        if (is_ipv6 && *tmp == ']') {
            tmp++;
            break;
        } else if (!is_ipv6 && (*tmp == ':' || *tmp == '/')) {
            break;
        }

        tmp++;
    }

    url_out->host = sentry__string_clonen(ptr, tmp - ptr);

    /* port */
    ptr = tmp;
    if (*ptr == ':') {
        ptr++;
        tmp = ptr;
        SKIP_WHILE_NOT(tmp, '/');
        aux_buf = sentry__string_clonen(ptr, tmp - ptr);
        char *end;
        url_out->port = (int)strtol(aux_buf, &end, 10);
        if (end != aux_buf + strlen(aux_buf)) {
            goto error;
        }
        sentry_free(aux_buf);
        aux_buf = 0;
        ptr = tmp;
    }

    if (!*ptr) {
        goto error;
    }

    /* end of netloc */
    if (*ptr != '/') {
        goto error;
    }

    /* path */
    tmp = ptr;
    SKIP_WHILE_NOT2(tmp, '#', '?');
    url_out->path = sentry__string_clonen(ptr, tmp - ptr);
    ptr = tmp;

    /* query */
    if (*ptr == '?') {
        ptr++;
        tmp = ptr;
        SKIP_WHILE_NOT(tmp, '#');
        url_out->query = sentry__string_clonen(ptr, tmp - ptr);
        ptr = tmp;
    }

    /* fragment */
    if (*ptr == '#') {
        ptr++;
        tmp = ptr;
        SKIP_WHILE_NOT(tmp, 0);
        url_out->fragment = sentry__string_clonen(ptr, tmp - ptr);
    }

    if (url_out->port == 0) {
        if (sentry__string_eq(url_out->scheme, "https")) {
            url_out->port = 443;
        } else if (sentry__string_eq(url_out->scheme, "http")) {
            url_out->port = 80;
        }
    }

    result = 0;
    goto cleanup;

error:
    result = 1;
    sentry__url_cleanup(url_out);
    goto cleanup;

cleanup:
    sentry_free(aux_buf);
    sentry_free(scratch);
    return result;
}

void
sentry__url_cleanup(sentry_url_t *url)
{
    sentry_free(url->scheme);
    sentry_free(url->host);
    sentry_free(url->path);
    sentry_free(url->query);
    sentry_free(url->fragment);
    sentry_free(url->username);
    sentry_free(url->password);
}

sentry_dsn_t *
sentry__dsn_new(const char *raw_dsn)
{
    sentry_url_t url;
    memset(&url, 0, sizeof(sentry_url_t));
    size_t path_len;
    char *tmp;
    char *end;

    sentry_dsn_t *dsn = SENTRY_MAKE(sentry_dsn_t);
    if (!dsn) {
        return NULL;
    }
    memset(dsn, 0, sizeof(sentry_dsn_t));
    dsn->refcount = 1;

    dsn->raw = sentry__string_clone(raw_dsn);
    if (!dsn->raw || !dsn->raw[0] || sentry__url_parse(&url, dsn->raw) != 0) {
        goto exit;
    }

    if (sentry__string_eq(url.scheme, "https")) {
        dsn->is_secure = 1;
    } else if (sentry__string_eq(url.scheme, "http")) {
        dsn->is_secure = 0;
    } else {
        goto exit;
    }

    dsn->host = url.host;
    url.host = NULL;
    dsn->public_key = url.username;
    url.username = NULL;
    dsn->secret_key = url.password;
    url.password = NULL;
    dsn->port = url.port;

    path_len = strlen(url.path);
    while (path_len > 0 && url.path[path_len - 1] == '/') {
        url.path[path_len - 1] = '\0';
        path_len--;
    }

    tmp = strrchr(url.path, '/');
    if (!tmp) {
        goto exit;
    }

    dsn->project_id = (uint64_t)strtoll(tmp + 1, &end, 10);
    if (end != tmp + strlen(tmp)) {
        goto exit;
    }
    *tmp = 0;
    dsn->path = url.path;
    url.path = NULL;
    dsn->is_valid = true;

exit:
    sentry__url_cleanup(&url);
    return dsn;
}

sentry_dsn_t *
sentry__dsn_incref(sentry_dsn_t *dsn)
{
    if (!dsn) {
        return NULL;
    }
    sentry__atomic_fetch_and_add(&dsn->refcount, 1);
    return dsn;
}

void
sentry__dsn_decref(sentry_dsn_t *dsn)
{
    if (!dsn) {
        return;
    }
    if (sentry__atomic_fetch_and_add(&dsn->refcount, -1) == 1) {
        sentry_free(dsn->raw);
        sentry_free(dsn->host);
        sentry_free(dsn->path);
        sentry_free(dsn->public_key);
        sentry_free(dsn->secret_key);
        sentry_free(dsn);
    }
}

char *
sentry__dsn_get_auth_header(const sentry_dsn_t *dsn)
{
    if (!dsn || !dsn->is_valid) {
        return NULL;
    }
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    sentry__stringbuilder_append(&sb, "Sentry sentry_key=");
    sentry__stringbuilder_append(&sb, dsn->public_key);
    sentry__stringbuilder_append(
        &sb, ", sentry_version=7, sentry_client=" SENTRY_SDK_USER_AGENT);
    return sentry__stringbuilder_into_string(&sb);
}

static void
init_string_builder_for_url(sentry_stringbuilder_t *sb, const sentry_dsn_t *dsn)
{
    sentry__stringbuilder_init(sb);
    sentry__stringbuilder_append(sb, dsn->is_secure ? "https" : "http");
    sentry__stringbuilder_append(sb, "://");
    sentry__stringbuilder_append(sb, dsn->host);
    sentry__stringbuilder_append(sb, ":");
    sentry__stringbuilder_append_int64(sb, (int64_t)dsn->port);
    sentry__stringbuilder_append(sb, dsn->path);
    sentry__stringbuilder_append(sb, "/api/");
    sentry__stringbuilder_append_int64(sb, (int64_t)dsn->project_id);
}

char *
sentry__dsn_get_envelope_url(const sentry_dsn_t *dsn)
{
    if (!dsn || !dsn->is_valid) {
        return NULL;
    }
    sentry_stringbuilder_t sb;
    init_string_builder_for_url(&sb, dsn);
    sentry__stringbuilder_append(&sb, "/envelope/");
    return sentry__stringbuilder_into_string(&sb);
}

char *
sentry__dsn_get_minidump_url(const sentry_dsn_t *dsn)
{
    if (!dsn || !dsn->is_valid) {
        return NULL;
    }
    sentry_stringbuilder_t sb;
    init_string_builder_for_url(&sb, dsn);
    sentry__stringbuilder_append(
        &sb, "/minidump/?sentry_client=" SENTRY_SDK_USER_AGENT "&sentry_key=");
    sentry__stringbuilder_append(&sb, dsn->public_key);
    return sentry__stringbuilder_into_string(&sb);
}

char *
sentry__msec_time_to_iso8601(uint64_t time)
{
    char buf[255];
    time_t secs = time / 1000;
    struct tm *tm;
#ifdef SENTRY_PLATFORM_WINDOWS
    tm = gmtime(&secs);
#else
    struct tm tm_buf;
    tm = gmtime_r(&secs, &tm_buf);
#endif
    size_t end = strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", tm);

    int msecs = time % 1000;
    if (msecs) {
        snprintf(buf + end, 10, ".%03d", msecs);
    }

    strcat(buf, "Z");
    return sentry__string_clone(buf);
}

uint64_t
sentry__iso8601_to_msec(const char *iso)
{
    size_t len = strlen(iso);
    if (len != 20 && len != 24) {
        return 0;
    }
    // The code is adapted from: https://stackoverflow.com/a/26896792
    int y, M, d, h, m, s, msec = 0;
    int consumed = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d%n", &y, &M, &d, &h, &m, &s, &consumed)
            < 6
        || consumed != 19) {
        return 0;
    }
    iso += consumed;
    // we optionally have millisecond precision
    if (iso[0] == '.') {
        if (sscanf(iso, ".%d%n", &msec, &consumed) < 1 || consumed != 4) {
            return 0;
        }
        iso += consumed;
    }
    // the string is terminated by `Z`
    if (iso[0] != 'Z') {
        return 0;
    }

    struct tm tm;
    tm.tm_year = y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = s;
#ifdef SENTRY_PLATFORM_WINDOWS
    time_t time = _mkgmtime(&tm);
#else
    time_t time = timegm(&tm);
#endif
    if (time == -1) {
        return 0;
    }

    return (uint64_t)time * 1000 + msec;
}

#ifdef SENTRY_PLATFORM_WINDOWS
#    define sentry__locale_t _locale_t
#else
#    define sentry__locale_t locale_t
#endif

// On NDK locales are not supported.  It defines `stdtod_l` as a function that
// forwards to `stdtod`, but it does not define `vsnprintf_l` sadly.  This means
// if Android ever adds locale support in NDK we will have to revisit this code
// to ensure the C locale is also used there.
#if !defined(SENTRY_PLATFORM_ANDROID) && !defined(SENTRY_PLATFORM_IOS)
static sentry__locale_t
c_locale()
{
    static long c_locale_initialized = 0;
    static sentry__locale_t c_locale;
    if (sentry__atomic_store(&c_locale_initialized, 1) == 0) {
#    ifdef SENTRY_PLATFORM_WINDOWS
        c_locale = _create_locale(LC_ALL, "C");
#    else
        c_locale = newlocale(LC_ALL_MASK, "C", (sentry__locale_t)0);
#    endif
    }
    return c_locale;
}
#endif

double
sentry__strtod_c(const char *ptr, char **endptr)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    return _strtod_l(ptr, endptr, c_locale());
#elif defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_IOS)
    return strtod(ptr, endptr);
#else
    return strtod_l(ptr, endptr, c_locale());
#endif
}

int
sentry__snprintf_c(char *buf, size_t buf_size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    int rv;
#ifdef SENTRY_PLATFORM_WINDOWS
    rv = _vsnprintf_l(buf, buf_size, fmt, c_locale(), args);
#elif defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_IOS)
    rv = vsnprintf(buf, buf_size, fmt, args);
#elif defined(SENTRY_PLATFORM_MACOS)
    rv = vsnprintf_l(buf, buf_size, c_locale(), fmt, args);
#else
    rv = stbsp_vsnprintf(buf, buf_size, fmt, args);
#endif

    va_end(args);
    return rv;
}
