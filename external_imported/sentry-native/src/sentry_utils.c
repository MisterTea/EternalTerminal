// According to http://lua-users.org/lists/lua-l/2016-04/msg00216.html we can
// use `stdtod_l` on all platforms when defining `_GNU_SOURCE`.

#include "sentry_boot.h"

#include "sentry_alloc.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_utils.h"

#include "sentry_random.h"

#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef SENTRY_PLATFORM_DARWIN
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
sentry__url_parse(sentry_url_t *url_out, const char *url, bool requires_path)
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
    url_out->scheme
        = sentry__string_clone_n_unchecked(ptr, (size_t)(tmp - ptr));

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
        url_out->username
            = sentry__string_clone_n_unchecked(ptr, (size_t)(tmp - ptr));
        ptr = tmp;
        if (*ptr == ':') {
            ptr++;
            tmp = ptr;
            SKIP_WHILE_NOT(tmp, '@');
            url_out->password
                = sentry__string_clone_n_unchecked(ptr, (size_t)(tmp - ptr));
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

    url_out->host = sentry__string_clone_n_unchecked(ptr, (size_t)(tmp - ptr));

    /* port */
    ptr = tmp;
    if (*ptr == ':') {
        ptr++;
        tmp = ptr;
        SKIP_WHILE_NOT(tmp, '/');
        aux_buf = sentry__string_clone_n_unchecked(ptr, (size_t)(tmp - ptr));
        char *end;
        url_out->port = (int)strtol(aux_buf, &end, 10);
        if (end != aux_buf + strlen(aux_buf)) {
            goto error;
        }
        sentry_free(aux_buf);
        aux_buf = 0;
        ptr = tmp;
    }

    if (url_out->port == 0) {
        if (sentry__string_eq(url_out->scheme, "https")) {
            url_out->port = 443;
        } else if (sentry__string_eq(url_out->scheme, "http")) {
            url_out->port = 80;
        }
    }

    if (!*ptr) {
        if (requires_path) {
            goto error;
        }
        result = 0;
        goto cleanup;
    }

    /* end of netloc */
    if (*ptr != '/') {
        goto error;
    }

    /* path */
    tmp = ptr;
    SKIP_WHILE_NOT2(tmp, '#', '?');
    url_out->path = sentry__string_clone_n_unchecked(ptr, (size_t)(tmp - ptr));
    ptr = tmp;

    /* query */
    if (*ptr == '?') {
        ptr++;
        tmp = ptr;
        SKIP_WHILE_NOT(tmp, '#');
        url_out->query
            = sentry__string_clone_n_unchecked(ptr, (size_t)(tmp - ptr));
        ptr = tmp;
    }

    /* fragment */
    if (*ptr == '#') {
        ptr++;
        tmp = ptr;
        SKIP_WHILE_NOT(tmp, 0);
        url_out->fragment
            = sentry__string_clone_n_unchecked(ptr, (size_t)(tmp - ptr));
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
    memset(url, 0, sizeof(sentry_url_t));
}

sentry_dsn_t *
sentry__dsn_new_n(const char *raw_dsn, size_t raw_dsn_len)
{
    sentry_url_t url;
    memset(&url, 0, sizeof(sentry_url_t));
    size_t path_len;
    char *project_id;
    // org_id is u64 in relay, so needs 20 characters + null termination
    char org_id[21] = "";

    sentry_dsn_t *dsn = SENTRY_MAKE(sentry_dsn_t);
    if (!dsn) {
        return NULL;
    }
    dsn->refcount = 1;

    dsn->raw = sentry__string_clone_n(raw_dsn, raw_dsn_len);
    if (!dsn->raw || !dsn->raw[0]
        || sentry__url_parse(&url, dsn->raw, true) != 0) {
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
    const char *org_id_dot = strchr(url.host, '.');
    if (org_id_dot && url.host[0] == 'o') {
        size_t length = (size_t)(org_id_dot - url.host - 1); // leave the o
        strncpy(org_id, url.host + 1, MIN(length, 20));
        org_id[MIN(length, 20)] = '\0'; // Null-terminate the string
        char *org_id_end_ptr;
        const unsigned long long int org_id_int
            = strtoull(org_id, &org_id_end_ptr, 10);
        // check if valid uint64 AND not actually the number 0
        if (length > 20
            || (org_id_int == 0ULL && org_id_end_ptr != org_id + length)) {
            memset(org_id, '\0', 20);
        }
    }
    dsn->org_id = sentry__string_clone(org_id);
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

    project_id = strrchr(url.path, '/');
    if (!project_id || strlen(project_id + 1) == 0) {
        goto exit;
    }

    dsn->project_id = sentry__string_clone(project_id + 1);
    *project_id = 0;

    dsn->path = url.path;
    url.path = NULL;

    if (dsn->public_key && dsn->host && dsn->path) {
        dsn->is_valid = true;
    }

exit:
    sentry__url_cleanup(&url);
    return dsn;
}

sentry_dsn_t *
sentry__dsn_new(const char *raw_dsn)
{
    if (!raw_dsn) {
        return NULL;
    }

    return sentry__dsn_new_n(raw_dsn, strlen(raw_dsn));
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
        sentry_free(dsn->org_id);
        sentry_free(dsn->path);
        sentry_free(dsn->public_key);
        sentry_free(dsn->secret_key);
        sentry_free(dsn->project_id);
        sentry_free(dsn);
    }
}

char *
sentry__dsn_get_auth_header(const sentry_dsn_t *dsn, const char *user_agent)
{
    if (!dsn || !dsn->is_valid) {
        return NULL;
    }
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    sentry__stringbuilder_append(&sb, "Sentry sentry_key=");
    sentry__stringbuilder_append(&sb, dsn->public_key);
    sentry__stringbuilder_append(&sb, ", sentry_version=7");

    sentry__stringbuilder_append(&sb, ", sentry_client=");
    if (user_agent) {
        sentry__stringbuilder_append(&sb, user_agent);
    } else {
        sentry__stringbuilder_append(&sb, SENTRY_SDK_USER_AGENT);
    }
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
    sentry__stringbuilder_append(sb, dsn->project_id);
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
sentry__dsn_get_upload_url(const sentry_dsn_t *dsn)
{
    if (!dsn || !dsn->is_valid) {
        return NULL;
    }
    sentry_stringbuilder_t sb;
    init_string_builder_for_url(&sb, dsn);
    sentry__stringbuilder_append(&sb, "/upload/");
    return sentry__stringbuilder_into_string(&sb);
}

char *
sentry__dsn_resolve_url(const sentry_dsn_t *dsn, const char *path)
{
    if (!dsn || !dsn->is_valid || !path) {
        return NULL;
    }
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    if (path[0] == '/') {
        sentry__stringbuilder_append(&sb, dsn->is_secure ? "https" : "http");
        sentry__stringbuilder_append(&sb, "://");
        sentry__stringbuilder_append(&sb, dsn->host);
        sentry__stringbuilder_append_char(&sb, ':');
        sentry__stringbuilder_append_int64(&sb, (int64_t)dsn->port);
    }
    sentry__stringbuilder_append(&sb, path);
    return sentry__stringbuilder_into_string(&sb);
}

char *
sentry__dsn_get_minidump_url(const sentry_dsn_t *dsn, const char *user_agent)
{
    if (!dsn || !dsn->is_valid || !user_agent) {
        return NULL;
    }
    sentry_stringbuilder_t sb;
    init_string_builder_for_url(&sb, dsn);
    sentry__stringbuilder_append(&sb, "/minidump/?sentry_client=");
    sentry__stringbuilder_append(&sb, user_agent);
    sentry__stringbuilder_append(&sb, "&sentry_key=");
    sentry__stringbuilder_append(&sb, dsn->public_key);
    return sentry__stringbuilder_into_string(&sb);
}

char *
sentry__base64_encode(const char *data, size_t len)
{
    static const char b64[]
        = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t output_len = 4 * ((len + 2) / 3);
    char *out = sentry_malloc(output_len + 1);
    if (!out) {
        return NULL;
    }

    size_t i, j;
    for (i = 0, j = 0; i < len; i += 3) {
        uint32_t triple = (uint32_t)(unsigned char)data[i] << 16;
        if (i + 1 < len) {
            triple |= (uint32_t)(unsigned char)data[i + 1] << 8;
        }
        if (i + 2 < len) {
            triple |= (uint32_t)(unsigned char)data[i + 2];
        }

        out[j++] = b64[(triple >> 18) & 0x3F];
        out[j++] = b64[(triple >> 12) & 0x3F];
        out[j++] = i + 1 < len ? b64[(triple >> 6) & 0x3F] : '=';
        out[j++] = i + 2 < len ? b64[triple & 0x3F] : '=';
    }

    out[j] = '\0';
    return out;
}

char *
sentry__usec_time_to_iso8601(uint64_t time)
{
    char buf[64];
    size_t buf_len = sizeof(buf);
    time_t secs = time / 1000000;
    struct tm *tm;
#if defined(SENTRY_PLATFORM_WINDOWS) || defined(SENTRY_PLATFORM_PS)
    tm = gmtime(&secs);
#else
    struct tm tm_buf;
    tm = gmtime_r(&secs, &tm_buf);
#endif
    // It might as well be that the `time` parameter is broken in some way and
    // would create a broken `tm` that then later causes formatting issues. We
    // have seen super strange timestamps in some event payloads.
    if (!tm || tm->tm_year > 9000) {
        return NULL;
    }
    size_t written = strftime(buf, buf_len, "%Y-%m-%dT%H:%M:%S", tm);
    if (written == 0) {
        return NULL;
    }

    int usecs = time % 1000000;
    if (usecs) {
        size_t rv = (size_t)snprintf(
            buf + written, buf_len - written, ".%06d", usecs);
        if (rv >= buf_len - written) {
            return NULL;
        }
        written += rv;
    }

    if (written + 2 > buf_len) {
        return NULL;
    }
    buf[written] = 'Z';
    buf[written + 1] = '\0';
    return sentry__string_clone(buf);
}

#ifndef SENTRY_PLATFORM_PS
uint64_t
sentry__iso8601_to_usec(const char *iso)
{
    size_t len = sentry__guarded_strlen(iso);
    if (len != 20 && len != 27) {
        return 0;
    }
    // The code is adapted from: https://stackoverflow.com/a/26896792
    int y, M, d, h, m, s, usec = 0;
    int consumed = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d%n", &y, &M, &d, &h, &m, &s, &consumed)
            < 6
        || consumed != 19) {
        return 0;
    }
    iso += consumed;
    // we optionally have microsecond precision
    if (iso[0] == '.') {
        if (sscanf(iso, ".%d%n", &usec, &consumed) < 1 || consumed != 7) {
            printf("consumed = %d\n", consumed);
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
#    ifdef SENTRY_PLATFORM_WINDOWS
    time_t time = _mkgmtime(&tm);
#    elif defined(SENTRY_PLATFORM_AIX)
    /*
     * timegm is a GNU extension that AIX doesn't support. We'll have to fake
     * it by setting TZ instead w/ mktime, then unsets it. Changes global env.
     */
    time_t time;
    char *tz_env;
    tz_env = getenv("TZ");
    if (tz_env) {
        /* make a copy of it, since it'll change when we set it to UTC */
        tz_env = strdup(tz_env);
    }
    setenv("TZ", "UTC", 1);
    tzset();
    time = mktime(&tm);
    /* revert */
    if (tz_env) {
        setenv("TZ", tz_env, 1);
        free(tz_env);
    } else {
        unsetenv("TZ");
    }
    tzset();
#    else
    time_t time = timegm(&tm);
#    endif
    if (time == -1) {
        return 0;
    }

    return (uint64_t)time * 1000000 + (uint64_t)usec;
}
#endif

#ifdef SENTRY_PLATFORM_WINDOWS
#    define sentry__locale_t _locale_t
#else
#    define sentry__locale_t locale_t
#endif

// On NDK locales are not supported.  It defines `stdtod_l` as a function that
// forwards to `stdtod`, but it does not define `vsnprintf_l` sadly.  This means
// if Android ever adds locale support in NDK we will have to revisit this code
// to ensure the C locale is also used there.
#if !defined(SENTRY_PLATFORM_ANDROID) && !defined(SENTRY_PLATFORM_IOS)         \
    && !defined(SENTRY_PLATFORM_AIX) && !defined(SENTRY_PLATFORM_NX)           \
    && !defined(SENTRY_PLATFORM_PS)
#    define HAS_C_LOCALE
#endif

#ifdef HAS_C_LOCALE
static sentry__locale_t
c_locale(void)
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
#elif !defined(HAS_C_LOCALE)
    return strtod(ptr, endptr);
#else
    return strtod_l(ptr, endptr, c_locale());
#endif
}

double
sentry__getenv_double(const char *name, double fallback)
{
    const char *str = getenv(name);
    if (!str) {
        return fallback;
    }
    char *end = NULL;
    double val = sentry__strtod_c(str, &end);
    if (end == str || !isfinite(val)) {
        return fallback;
    }
    if (end[strspn(end, " \t\r\n")] != '\0') {
        return fallback;
    }
    return val;
}

int
sentry__snprintf_c(char *buf, size_t buf_size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    int rv;
#ifdef SENTRY_PLATFORM_WINDOWS
    rv = _vsnprintf_l(buf, buf_size, fmt, c_locale(), args);
#elif !defined(HAS_C_LOCALE)
    rv = vsnprintf(buf, buf_size, fmt, args);
#elif defined(SENTRY_PLATFORM_DARWIN)
    rv = vsnprintf_l(buf, buf_size, c_locale(), fmt, args);
#else
    rv = stbsp_vsnprintf(buf, buf_size, fmt, args);
#endif

    va_end(args);
    return rv;
}

bool
sentry__check_min_version(sentry_version_t actual, sentry_version_t expected)
{
    if (actual.major < expected.major) {
        return false;
    }
    if (actual.major == expected.major && actual.minor < expected.minor) {
        return false;
    }
    if (actual.major == expected.major && actual.minor == expected.minor
        && actual.patch < expected.patch) {
        return false;
    }

    return true;
}

void
sentry__generate_sample_rand(sentry_value_t context)
{
    uint64_t rnd;
    double sample_rand = 1.0;
    do {
        sentry__getrandom(&rnd, sizeof(rnd));
        sample_rand = (double)rnd / (double)UINT64_MAX;
    } while (sample_rand == 1.0); // re-roll when generating 1.0
    sentry_value_set_by_key(
        context, "sample_rand", sentry_value_new_double(sample_rand));
}

bool
sentry__baggage_iter_next(
    sentry_slice_t *remaining, sentry_slice_t *key, sentry_slice_t *value)
{
    while (remaining->len > 0) {
        size_t comma = sentry__slice_find(*remaining, ',');
        sentry_slice_t member;
        if (comma == (size_t)-1) {
            member = *remaining;
            *remaining = sentry__slice_advance(*remaining, remaining->len);
        } else {
            member = (sentry_slice_t) { remaining->ptr, comma };
            *remaining = sentry__slice_advance(*remaining, comma + 1);
        }
        member = sentry__slice_trim(member);

        size_t eq = sentry__slice_find(member, '=');
        if (eq == (size_t)-1) {
            continue;
        }
        sentry_slice_t k
            = sentry__slice_trim((sentry_slice_t) { member.ptr, eq });
        if (k.len == 0) {
            continue;
        }
        sentry_slice_t v = { member.ptr + eq + 1, member.len - eq - 1 };
        size_t semi = sentry__slice_find(v, ';');
        if (semi != (size_t)-1) {
            v.len = semi;
        }
        *key = k;
        *value = sentry__slice_trim(v);
        return true;
    }
    return false;
}

size_t
sentry__percent_decode_inplace(char *s, size_t len)
{
    size_t r = 0;
    size_t w = 0;
    while (r < len) {
        if (s[r] == '%' && r + 2 < len && isxdigit((unsigned char)s[r + 1])
            && isxdigit((unsigned char)s[r + 2])) {
            char hex[3] = { s[r + 1], s[r + 2], '\0' };
            s[w++] = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            s[w++] = s[r++];
        }
    }
    return w;
}
