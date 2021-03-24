#ifndef SENTRY_UTILS_H_INCLUDED
#define SENTRY_UTILS_H_INCLUDED

#include "sentry_boot.h"

#ifdef SENTRY_PLATFORM_WINDOWS
#    include <winnt.h>
#else
#    include <sys/time.h>
#    include <time.h>
#endif

/**
 * This represents a URL parsed into its different parts.
 */
typedef struct {
    char *scheme;
    char *host;
    int port;
    char *path;
    char *query;
    char *fragment;
    char *username;
    char *password;
} sentry_url_t;

/**
 * Parse the given `url` into the pre-allocated `url_out` parameter.
 * Returns 0 on success.
 */
int sentry__url_parse(sentry_url_t *url_out, const char *url);

/**
 * This will free all the internal members of `url`, but not `url` itself, as
 * that might have been stack allocated.
 */
void sentry__url_cleanup(sentry_url_t *url);

/**
 * This is the internal representation of a parsed DSN.
 */
typedef struct sentry_dsn_s {
    char *raw;
    char *host;
    char *path;
    char *secret_key;
    char *public_key;
    uint64_t project_id;
    int port;
    long refcount;
    bool is_valid;
    bool is_secure;
} sentry_dsn_t;

/**
 * This will parse the DSN URL given in `dsn`.
 *
 * The returned `sentry_dsn_t` will have have its `is_valid` flag set when the
 * DSN has been successfully parsed.
 */
sentry_dsn_t *sentry__dsn_new(const char *dsn);

/**
 * Increases the reference-count of the DSN.
 */
sentry_dsn_t *sentry__dsn_incref(sentry_dsn_t *dsn);

/**
 * Decrements the reference-count of the DSN.
 */
void sentry__dsn_decref(sentry_dsn_t *dsn);

/**
 * This will create a new string, with the contents of the `X-Sentry-Auth`, as
 * described here:
 * https://docs.sentry.io/development/sdk-dev/overview/#authentication
 */
char *sentry__dsn_get_auth_header(const sentry_dsn_t *dsn);

/**
 * Returns the envelope endpoint url used for normal uploads as a newly
 * allocated string.
 */
char *sentry__dsn_get_envelope_url(const sentry_dsn_t *dsn);

/**
 * Returns the minidump endpoint url used for uploads done by the out-of-process
 * crashpad backend as a newly allocated string.
 */
char *sentry__dsn_get_minidump_url(const sentry_dsn_t *dsn);

/**
 * Returns the number of milliseconds since the unix epoch.
 */
static inline uint64_t
sentry__msec_time(void)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    // Contains a 64-bit value representing the number of 100-nanosecond
    // intervals since January 1, 1601 (UTC).
    FILETIME file_time;
    SYSTEMTIME system_time;
    GetSystemTime(&system_time);
    SystemTimeToFileTime(&system_time, &file_time);

    uint64_t timestamp = (uint64_t)file_time.dwLowDateTime
        + ((uint64_t)file_time.dwHighDateTime << 32);
    timestamp -= 116444736000000000LL; // convert to unix epoch
    timestamp /= 10000LL; // 100ns -> 1ms

    return timestamp;
#else
    struct timeval tv;
    return (gettimeofday(&tv, NULL) == 0)
        ? (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000
        : 0;
#endif
}

/**
 * Returns a monotonic millisecond resolution time.
 *
 * This should be used for timeouts and similar.
 * For timestamps, use `sentry__msec_time` instead.
 */
static inline uint64_t
sentry__monotonic_time(void)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    static LARGE_INTEGER qpc_frequency = { { 0, 0 } };

    if (!qpc_frequency.QuadPart) {
        QueryPerformanceFrequency(&qpc_frequency);
    }

    // Fallback to GetTickCount() on QPC fail
    if (!qpc_frequency.QuadPart) {
#    if _WIN32_WINNT >= 0x0600
        return GetTickCount64();
#    else
        return GetTickCount();
#    endif
    }

    LARGE_INTEGER qpc_counter;
    QueryPerformanceCounter(&qpc_counter);
    return qpc_counter.QuadPart * 1000 / qpc_frequency.QuadPart;
#else
    struct timespec tv;
    return (clock_gettime(CLOCK_MONOTONIC, &tv) == 0)
        ? (uint64_t)tv.tv_sec * 1000 + tv.tv_nsec / 1000000
        : 0;
#endif
}

/**
 * Formats a timestamp (milliseconds since epoch) into ISO8601 format.
 */
char *sentry__msec_time_to_iso8601(uint64_t time);

/**
 * Parses a ISO8601 formatted string into a millisecond resolution timestamp.
 * This only accepts the format `YYYY-MM-DD'T'hh:mm:ss(.zzz)'Z'`, which is
 * produced by the `sentry__msec_time_to_iso8601` function.
 */
uint64_t sentry__iso8601_to_msec(const char *iso);

/**
 * Locale independent (or rather, using "C" locale) `strtod`.
 */
double sentry__strtod_c(const char *ptr, char **endptr);

/**
 * Locale independent (or rather, using "C" locale) `snprintf`.
 */
int sentry__snprintf_c(char *buf, size_t buf_size, const char *fmt, ...);

#endif
