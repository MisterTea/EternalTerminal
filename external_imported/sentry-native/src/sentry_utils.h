#ifndef SENTRY_UTILS_H_INCLUDED
#define SENTRY_UTILS_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_slice.h"

#ifdef SENTRY_PLATFORM_DARWIN
#    include <mach/clock.h>
#    include <mach/mach.h>
#endif
#ifdef SENTRY_PLATFORM_WINDOWS
#    include "sentry_os.h"
#    include <winnt.h>
#else
#    include <sys/time.h>
#    include <time.h>
#endif

#ifdef SENTRY_PLATFORM_PS
#    undef MIN
#    undef MAX
#    define getenv(_) NULL
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/**
 * Byte-swap the first three GUID fields (Data1, Data2, Data3) of a 16-byte
 * UUID in place.  This converts between RFC 4122 (big-endian) and Windows
 * mixed-endian GUID layout, which is used by RSDS CodeView records and Sentry
 * debug-id strings.
 */
static inline void
sentry__uuid_swap_guid_bytes(void *uuid)
{
    uint8_t *p = (uint8_t *)uuid;
    uint8_t t;
    // Swap Data1 (4 bytes)
    t = p[0];
    p[0] = p[3];
    p[3] = t;
    t = p[1];
    p[1] = p[2];
    p[2] = t;
    // Swap Data2 (2 bytes)
    t = p[4];
    p[4] = p[5];
    p[5] = t;
    // Swap Data3 (2 bytes)
    t = p[6];
    p[6] = p[7];
    p[7] = t;
}

#if defined(_MSC_VER) && !defined(__clang__)
#    define UNREACHABLE(reason) assert(!reason)
#else
#    define UNREACHABLE(reason) assert(!(bool)reason)
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
 * `requires_path` flags whether the url needs a / after the host(:port) section
 */
int sentry__url_parse(
    sentry_url_t *url_out, const char *url, bool requires_path);

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
    // TODO after u64 value type, change to uint64_t
    //      -> how to handle empty org_id then?
    char *org_id;
    char *path;
    char *secret_key;
    char *public_key;
    char *project_id;
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
sentry_dsn_t *sentry__dsn_new_n(const char *dsn, size_t raw_dsn_len);

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
char *sentry__dsn_get_auth_header(
    const sentry_dsn_t *dsn, const char *user_agent);

/**
 * Returns the envelope endpoint url used for normal uploads as a newly
 * allocated string.
 */
char *sentry__dsn_get_envelope_url(const sentry_dsn_t *dsn);

/**
 * Returns the TUS upload endpoint url as a newly allocated string.
 */
char *sentry__dsn_get_upload_url(const sentry_dsn_t *dsn);

/**
 * Resolves a possibly relative URL against the DSN's origin.
 * If the path starts with '/', the DSN's scheme, host, and port are prepended.
 * Returns a newly allocated string.
 */
char *sentry__dsn_resolve_url(const sentry_dsn_t *dsn, const char *path);

/**
 * Returns the minidump endpoint url used for uploads done by the out-of-process
 * crashpad backend as a newly allocated string.
 */
char *sentry__dsn_get_minidump_url(
    const sentry_dsn_t *dsn, const char *user_agent);

/**
 * RFC 4648 base64-encodes `len` bytes from `data` and returns a newly
 * allocated NUL-terminated string (caller frees).
 */
char *sentry__base64_encode(const char *data, size_t len);

/**
 * Returns the number of microseconds since the unix epoch.
 */
static inline uint64_t
sentry__usec_time(void)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    // Contains a 64-bit value representing the number of 100-nanosecond
    // intervals since January 1, 1601 (UTC).
    FILETIME file_time;
    sentry__get_system_time(&file_time);
    uint64_t timestamp = (uint64_t)file_time.dwLowDateTime
        + ((uint64_t)file_time.dwHighDateTime << 32);
    timestamp -= 116444736000000000LL; // convert to unix epoch
    timestamp /= 10LL; // 100ns -> 1us

    return timestamp;
#else
    struct timeval tv;
    return (gettimeofday(&tv, NULL) == 0)
        ? (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec
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
    return (uint64_t)qpc_counter.QuadPart * 1000
        / (uint64_t)qpc_frequency.QuadPart;
#elif defined(SENTRY_PLATFORM_DARWIN)

// try `clock_gettime` first if available,
// fall back to `host_get_clock_service` otherwise
#    if defined(MAC_OS_X_VERSION_10_12) && __has_builtin(__builtin_available)
    if (__builtin_available(macOS 10.12, *)) {
        struct timespec tv;
        return (clock_gettime(CLOCK_MONOTONIC, &tv) == 0)
            ? (uint64_t)tv.tv_sec * 1000 + tv.tv_nsec / 1000000
            : 0;
    }
#    endif

    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    return (uint64_t)mts.tv_sec * 1000 + mts.tv_nsec / 1000000;
#else
    struct timespec tv;
    return (clock_gettime(CLOCK_MONOTONIC, &tv) == 0)
        ? (uint64_t)tv.tv_sec * 1000 + tv.tv_nsec / 1000000
        : 0;
#endif
}

/**
 * Formats a timestamp (microseconds since epoch) into ISO8601 format.
 */
char *sentry__usec_time_to_iso8601(uint64_t time);

/**
 * Parses a ISO8601 formatted string into a microsecond resolution timestamp.
 * This only accepts the format `YYYY-MM-DD'T'hh:mm:ss(.zzzzzz)'Z'`, which is
 * produced by the `sentry__usec_time_to_iso8601` function.
 */
uint64_t sentry__iso8601_to_usec(const char *iso);

/**
 * Locale independent (or rather, using "C" locale) `strtod`.
 */
double sentry__strtod_c(const char *ptr, char **endptr);

/**
 * Reads a double from the environment variable `name`.
 * Returns `fallback` if the variable is unset, empty, or not a finite number.
 * Leading and trailing whitespace is tolerated.
 */
double sentry__getenv_double(const char *name, double fallback);

/**
 * Locale independent (or rather, using "C" locale) `snprintf`.
 */
int sentry__snprintf_c(char *buf, size_t buf_size, const char *fmt, ...);

/**
 * Represents a version of a software artifact.
 */
typedef struct {
    unsigned int major;
    unsigned int minor;
    unsigned int patch;
} sentry_version_t;

/**
 * Checks whether `actual` is the same or a later version than `expected`.
 * Returns `true` if that is the case.
 */
bool sentry__check_min_version(
    sentry_version_t actual, sentry_version_t expected);

/**
 * Generates and sets a sample_rand value on the given context.
 * The value set will be in range [0.0, 1.0)
 */
void sentry__generate_sample_rand(sentry_value_t context);

/**
 * Yields the next W3C Baggage member from `remaining`, advancing it past the
 * yielded member. `key` and `value` are borrowed slices into the original
 * buffer with surrounding whitespace trimmed; any property suffix (`;...`)
 * after the value is stripped. Values are not percent-decoded; use
 * `sentry__percent_decode_inplace` on a mutable copy if needed.
 *
 * Malformed members (missing `=`, empty key) are skipped silently. Returns
 * false when `remaining` is exhausted.
 */
bool sentry__baggage_iter_next(
    sentry_slice_t *remaining, sentry_slice_t *key, sentry_slice_t *value);

/**
 * Decodes `%XX` percent-escapes in the first `len` bytes of `s` in place.
 * Malformed escapes (non-hex or truncated at the end) are passed through
 * verbatim. Returns the new length; the caller is responsible for writing a
 * terminating NUL if one is required.
 */
size_t sentry__percent_decode_inplace(char *s, size_t len);

#endif
