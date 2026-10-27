/**
 * sentry-native
 *
 * sentry-native is a C client to send events to native from
 * C and C++ applications.  It can work together with breakpad/crashpad
 * but also send events on its own.
 *
 * NOTE on encodings:
 *
 * Sentry will assume an encoding of UTF-8 for all string data that is captured
 * and being sent to sentry as an Event.
 * All the functions that are dealing with *paths* will assume an OS-specific
 * encoding. Concretely:
 * - UTF-8 on macOS (at least on the versions the Native SDK supports)
 * - very likely UTF-8 on Android since it defaults to UTF-8 everywhere
 * - the locale encoding on Linux and other UNIXes
 * While the above are configurable, they consistently use the encoding along
 * the entire path processing allowing us to consider paths as almost entirely
 * opaque. On Windows, the situation is much more complex. This is because
 * Windows has
 * - locale-dependent multibyte encoding in the CRT,
 * - ANSI codepage-dependent multibyte encoding in the console and against the
 *   A-functions of the Win32 API,
 * - which can be configured independently by the application but also on the
 *   system level.
 * The above makes it very hard for us to track which kind of encoding to
 * expect (and often it isn't clear to the application developer either).
 * Moreover, in contrast to the UNIXes at certain points we have to make the
 * call (consider logging and serialization/persistence of paths as well as when
 * handling paths of modules and when symbolizing stack frames).
 * Thus, on Windows the Native SDK canonicalizes narrow character strings for
 * paths to be UTF-8 to prevent any ANSI code-page dependency. We also provide
 * public wide string APIs for paths, which we recommend if your narrow string
 * paths are not guaranteed to be UTF-8 encoded. Internally, we handle all
 * Windows paths as UTF-8 and only convert them to wide string if required
 * at the Win32 boundary (any of W-functions, we only use those) of the SDK.
 *
 * NOTE on attachments:
 *
 * Attachments are read lazily at the time of `sentry_capture_event`,
 * `sentry_capture_event_with_scope`, or at the time of a hard crash. Relative
 * attachment paths will be resolved according to the current working directory
 * at the time of envelope creation. When adding and removing attachments, they
 * are matched according to their given `path`. No normalization is performed.
 */

#ifndef SENTRY_H_INCLUDED
#define SENTRY_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* common platform detection */
#ifdef _WIN32
#    define SENTRY_PLATFORM_WINDOWS
#    ifdef _GAMING_XBOX
#        define SENTRY_PLATFORM_XBOX
#    elif defined(_GAMING_DESKTOP)
#        define SENTRY_PLATFORM_WINGDK
#    endif
#elif defined(__APPLE__)
#    include <TargetConditionals.h>
#    if defined(TARGET_OS_OSX) && TARGET_OS_OSX
#        define SENTRY_PLATFORM_MACOS
#    elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#        define SENTRY_PLATFORM_IOS
#    endif
#    define SENTRY_PLATFORM_DARWIN
#    define SENTRY_PLATFORM_UNIX
#elif defined(__ANDROID__)
#    define SENTRY_PLATFORM_ANDROID
#    define SENTRY_PLATFORM_LINUX
#    define SENTRY_PLATFORM_UNIX
#elif defined(__PROSPERO__)
#    define SENTRY_PLATFORM_PS
#    define SENTRY_PLATFORM_UNIX
#elif defined(__linux) || defined(__linux__)
#    define SENTRY_PLATFORM_LINUX
#    define SENTRY_PLATFORM_UNIX
#elif defined(_AIX)
/* IBM i PASE is also counted as AIX */
#    define SENTRY_PLATFORM_AIX
#    define SENTRY_PLATFORM_UNIX
#elif defined(__NINTENDO__)
#    define SENTRY_PLATFORM_NX
#else
#    error unsupported platform
#endif

/* SDK Version */
#ifndef SENTRY_SDK_NAME
#    if defined(SENTRY_PLATFORM_ANDROID)
#        define SENTRY_SDK_NAME "sentry.native.android"
#    elif defined(SENTRY_PLATFORM_XBOX)
#        define SENTRY_SDK_NAME "sentry.native.xbox"
#    elif defined(SENTRY_PLATFORM_WINGDK)
#        define SENTRY_SDK_NAME "sentry.native.wingdk"
#    else
#        define SENTRY_SDK_NAME "sentry.native"
#    endif
#endif
#ifndef SENTRY_SDK_VERSION
#    define SENTRY_SDK_VERSION "0.15.3"
#endif
#define SENTRY_SDK_USER_AGENT SENTRY_SDK_NAME "/" SENTRY_SDK_VERSION

/* marks a function as part of the sentry API */
#ifndef SENTRY_API
#    ifdef _WIN32
#        if defined(SENTRY_BUILD_SHARED) /* build dll */
#            define SENTRY_API __declspec(dllexport)
#        elif !defined(SENTRY_BUILD_STATIC) /* use dll */
#            define SENTRY_API __declspec(dllimport)
#        else /* static library */
#            define SENTRY_API
#        endif
#    else
#        if __GNUC__ >= 4
#            define SENTRY_API __attribute__((visibility("default")))
#        else
#            define SENTRY_API
#        endif
#    endif
#endif

#ifdef __has_attribute
#    if __has_attribute(deprecated)
#        define SENTRY_DEPRECATED(msg) __attribute__((deprecated(msg)))
#    endif
#endif
#ifndef SENTRY_DEPRECATED
#    if defined(__GNUC__)                                                      \
        && (__GNUC__ > 4                                                       \
            || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5)) /* GCC 4.5 */
#        define SENTRY_DEPRECATED(msg) __attribute__((deprecated(msg)))
#    elif defined(__clang__) && __clang__major__ >= 3 /* Clang 3.0 */
#        define SENTRY_DEPRECATED(msg) __attribute__((deprecated(msg)))
#    elif defined(_MSC_VER) && _MSC_VER >= 1400 /* VS 2005 (8.0) */
#        define SENTRY_DEPRECATED(msg) __declspec(deprecated(msg))
#    else
#        define SENTRY_DEPRECATED(msg)
#    endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#    define SENTRY_SUPPRESS_DEPRECATED                                         \
        _Pragma("GCC diagnostic push");                                        \
        _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#    define SENTRY_RESTORE_DEPRECATED _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
#    define SENTRY_SUPPRESS_DEPRECATED                                         \
        __pragma(warning(push));                                               \
        __pragma(warning(disable : 4996))
#    define SENTRY_RESTORE_DEPRECATED __pragma(warning(pop))
#else
#    define SENTRY_SUPPRESS_DEPRECATED
#    define SENTRY_RESTORE_DEPRECATED
#endif

/* marks a function as experimental api */
#ifndef SENTRY_EXPERIMENTAL_API
#    define SENTRY_EXPERIMENTAL_API SENTRY_API
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>

/* context type dependencies */
#ifdef _WIN32
#    include <windows.h>
#elif defined(SENTRY_PLATFORM_PS)
#    include <sys/signal.h>
#else
#    include <signal.h>
#endif

/**
 * The library internally uses the system malloc and free functions to manage
 * memory.  It does not use realloc.  The reason for this is that on unix
 * platforms we fall back to a simplistic page allocator once we have
 * encountered a SIGSEGV or another terminating signal as malloc is no longer
 * safe to use.  Since we cannot portably reallocate allocations made on the
 * pre-existing allocator, we're instead not using realloc.
 *
 * Note also that after SIGSEGV sentry_free() becomes a noop.
 */

/**
 * Allocates memory with the underlying allocator.
 */
SENTRY_API void *sentry_malloc(size_t size);

/**
 * Releases memory allocated from the underlying allocator.
 */
SENTRY_API void sentry_free(void *ptr);

/**
 * Legacy function.  Alias for `sentry_free`.
 */
#define sentry_string_free sentry_free

/* -- Protocol Value API -- */

/**
 * Type of sentry value.
 */
typedef enum {
    SENTRY_VALUE_TYPE_NULL,
    SENTRY_VALUE_TYPE_BOOL,
    SENTRY_VALUE_TYPE_INT32,
    SENTRY_VALUE_TYPE_INT64,
    SENTRY_VALUE_TYPE_UINT64,
    SENTRY_VALUE_TYPE_DOUBLE,
    SENTRY_VALUE_TYPE_STRING,
    SENTRY_VALUE_TYPE_LIST,
    SENTRY_VALUE_TYPE_OBJECT,
} sentry_value_type_t;

/**
 * Represents a sentry protocol value.
 *
 * The members of this type should never be accessed.  They are only here
 * so that alignment for the type can be properly determined.
 *
 * Values must be released with `sentry_value_decref`.  This lowers the
 * internal refcount by one.  If the refcount hits zero, it's freed.  Some
 * values like primitives have no refcount (like null), so operations on
 * those are no-ops.
 *
 * In addition, values can be frozen.  Some values like primitives are always
 * frozen, but lists and dicts are not and can be frozen on demand.  This
 * automatically happens for some shared values in the event payload like
 * the module list.
 */
union sentry_value_u {
    uint64_t _bits;
    double _double;
};
typedef union sentry_value_u sentry_value_t;

/**
 * Increments the reference count on the value.
 */
SENTRY_API sentry_value_t sentry_value_incref(sentry_value_t value);

/**
 * Decrements the reference count on the value.
 * Returns 0 if the value was freed or is a primitive (no tracking needed),
 * or non-zero if it still has references.
 */
SENTRY_API int sentry_value_decref(sentry_value_t value);

/**
 * Returns the refcount of a value.
 */
SENTRY_API size_t sentry_value_refcount(sentry_value_t value);

/**
 * Freezes a value.
 */
SENTRY_API void sentry_value_freeze(sentry_value_t value);

/**
 * Checks if a value is frozen.
 */
SENTRY_API int sentry_value_is_frozen(sentry_value_t value);

/**
 * Creates a null value.
 */
SENTRY_API sentry_value_t sentry_value_new_null(void);

/**
 * Creates a new 32-bit signed integer value.
 */
SENTRY_API sentry_value_t sentry_value_new_int32(int32_t value);

/**
 * Creates a new 64-bit signed integer value.
 */
SENTRY_API sentry_value_t sentry_value_new_int64(int64_t value);

/**
 * Creates a new 64-bit unsigned integer value.
 */
SENTRY_API sentry_value_t sentry_value_new_uint64(uint64_t value);

/**
 * Creates a new double value.
 */
SENTRY_API sentry_value_t sentry_value_new_double(double value);

/**
 * Creates a new boolean value.
 */
SENTRY_API sentry_value_t sentry_value_new_bool(int value);

/**
 * Creates a new null terminated string.
 */
SENTRY_API sentry_value_t sentry_value_new_string(const char *value);
SENTRY_API sentry_value_t sentry_value_new_string_n(
    const char *value, size_t value_len);

/**
 * Creates a new list value.
 */
SENTRY_API sentry_value_t sentry_value_new_list(void);

/**
 * Creates a new object.
 */
SENTRY_API sentry_value_t sentry_value_new_object(void);
/**
 * Creates a new user object.
 * Will return a sentry_value_new_null if all parameters are null.
 *
 * This DOES NOT set the user object, this should still be done with
 * sentry_set_user(), passing the return of this function as a parameter
 */
SENTRY_API sentry_value_t sentry_value_new_user(const char *id,
    const char *username, const char *email, const char *ip_address);
SENTRY_API sentry_value_t sentry_value_new_user_n(const char *id, size_t id_len,
    const char *username, size_t username_len, const char *email,
    size_t email_len, const char *ip_address, size_t ip_address_len);

/**
 * Measurement units for telemetry.
 *
 * These constants represent the standardized units supported by Sentry.
 * Custom units can also be passed as arbitrary strings.
 *
 * See: https://develop.sentry.dev/sdk/telemetry/attributes/#units
 */

/* Duration units */
#define SENTRY_UNIT_NANOSECOND "nanosecond"
#define SENTRY_UNIT_MICROSECOND "microsecond"
#define SENTRY_UNIT_MILLISECOND "millisecond"
#define SENTRY_UNIT_SECOND "second"
#define SENTRY_UNIT_MINUTE "minute"
#define SENTRY_UNIT_HOUR "hour"
#define SENTRY_UNIT_DAY "day"
#define SENTRY_UNIT_WEEK "week"

/* Information units */
#define SENTRY_UNIT_BIT "bit"
#define SENTRY_UNIT_BYTE "byte"
#define SENTRY_UNIT_KILOBYTE "kilobyte"
#define SENTRY_UNIT_KIBIBYTE "kibibyte"
#define SENTRY_UNIT_MEGABYTE "megabyte"
#define SENTRY_UNIT_MEBIBYTE "mebibyte"
#define SENTRY_UNIT_GIGABYTE "gigabyte"
#define SENTRY_UNIT_GIBIBYTE "gibibyte"
#define SENTRY_UNIT_TERABYTE "terabyte"
#define SENTRY_UNIT_TEBIBYTE "tebibyte"
#define SENTRY_UNIT_PETABYTE "petabyte"
#define SENTRY_UNIT_PEBIBYTE "pebibyte"
#define SENTRY_UNIT_EXABYTE "exabyte"
#define SENTRY_UNIT_EXBIBYTE "exbibyte"

/* Fraction units */
#define SENTRY_UNIT_RATIO "ratio"
#define SENTRY_UNIT_PERCENT "percent"

/**
 * Creates a new attribute object.
 *  value is required, unit is optional, but has to be one of the
 * `SENTRY_UNIT_X` macros.
 *
 * value must be a bool, int, double or string `sentry_value_t`
 * OR a list of bool, int, double or string (with all items being the same type)
 *
 * Moves ownership of value into the object. The caller does not
 * have to call `sentry_value_decref` on it.
 */
SENTRY_API sentry_value_t sentry_value_new_attribute(
    sentry_value_t value, const char *unit);
SENTRY_API sentry_value_t sentry_value_new_attribute_n(
    sentry_value_t value, const char *unit, size_t unit_len);

/**
 * Returns the type of the value passed.
 */
SENTRY_API sentry_value_type_t sentry_value_get_type(sentry_value_t value);

/**
 * Sets a key to a value in the map.
 *
 * This moves the ownership of the value into the map.  The caller does not
 * have to call `sentry_value_decref` on it.
 */
SENTRY_API int sentry_value_set_by_key(
    sentry_value_t value, const char *k, sentry_value_t v);

SENTRY_API int sentry_value_set_by_key_n(
    sentry_value_t value, const char *k, size_t k_len, sentry_value_t v);

/**
 * This removes a value from the map by key.
 */
SENTRY_API int sentry_value_remove_by_key(sentry_value_t value, const char *k);
SENTRY_API int sentry_value_remove_by_key_n(
    sentry_value_t value, const char *k, size_t k_len);

/**
 * Appends a value to a list.
 *
 * This moves the ownership of the value into the list.  The caller does not
 * have to call `sentry_value_decref` on it.
 */
SENTRY_API int sentry_value_append(sentry_value_t value, sentry_value_t v);

/**
 * Inserts a value into the list at a certain position.
 *
 * This moves the ownership of the value into the list.  The caller does not
 * have to call `sentry_value_decref` on it.
 *
 * If the list is shorter than the given index, it's automatically extended
 * and filled with `null` values.
 */
SENTRY_API int sentry_value_set_by_index(
    sentry_value_t value, size_t index, sentry_value_t v);

/**
 * This removes a value from the list by index.
 */
SENTRY_API int sentry_value_remove_by_index(sentry_value_t value, size_t index);

/**
 * Looks up a value in a map by key. If missing, a null value is returned.
 * The returned value is borrowed.
 */
SENTRY_API sentry_value_t sentry_value_get_by_key(
    sentry_value_t value, const char *k);
SENTRY_API sentry_value_t sentry_value_get_by_key_n(
    sentry_value_t value, const char *k, size_t k_len);

/**
 * Looks up a value in a map by key. If missing, a null value is returned.
 * The returned value is owned.
 *
 * If the caller no longer needs the value, it must be released with
 * `sentry_value_decref`.
 */
SENTRY_API sentry_value_t sentry_value_get_by_key_owned(
    sentry_value_t value, const char *k);
SENTRY_API sentry_value_t sentry_value_get_by_key_owned_n(
    sentry_value_t value, const char *k, size_t k_len);

/**
 * Looks up a value in a list by index. If missing, a null value is returned.
 * The returned value is borrowed.
 */
SENTRY_API sentry_value_t sentry_value_get_by_index(
    sentry_value_t value, size_t index);

/**
 * Looks up a value in a list by index. If missing, a null value is
 * returned. The returned value is owned.
 *
 * If the caller no longer needs the value, it must be released with
 * `sentry_value_decref`.
 */
SENTRY_API sentry_value_t sentry_value_get_by_index_owned(
    sentry_value_t value, size_t index);

/**
 * Returns the length of the given map or list.
 *
 * If an item is not a list or map, the return value is 0.
 */
SENTRY_API size_t sentry_value_get_length(sentry_value_t value);

/**
 * Converts a value into a 32bit signed integer.
 */
SENTRY_API int32_t sentry_value_as_int32(sentry_value_t value);

/**
 * Converts a value into a 64-bit signed integer.
 */
SENTRY_API int64_t sentry_value_as_int64(sentry_value_t value);

/**
 * Converts a value into a 64-bit unsigned integer.
 */
SENTRY_API uint64_t sentry_value_as_uint64(sentry_value_t value);

/**
 * Converts a value into a double value.
 */
SENTRY_API double sentry_value_as_double(sentry_value_t value);

/**
 * Returns the value as c string.
 */
SENTRY_API const char *sentry_value_as_string(sentry_value_t value);

/**
 * Returns `true` if the value is boolean true.
 */
SENTRY_API int sentry_value_is_true(sentry_value_t value);

/**
 * Returns `true` if the value is null.
 */
SENTRY_API int sentry_value_is_null(sentry_value_t value);

/**
 * Serialize a sentry value to JSON.
 *
 * The string is freshly allocated and must be freed with
 * `sentry_free`.
 */
SENTRY_API char *sentry_value_to_json(sentry_value_t value);

/**
 * Sentry levels for events and breadcrumbs.
 */
typedef enum sentry_level_e {
    SENTRY_LEVEL_TRACE = -2,
    SENTRY_LEVEL_DEBUG = -1,
    SENTRY_LEVEL_INFO = 0,
    SENTRY_LEVEL_WARNING = 1,
    SENTRY_LEVEL_ERROR = 2,
    SENTRY_LEVEL_FATAL = 3,
} sentry_level_t;

/**
 * Creates a new empty Event value.
 *
 * See https://docs.sentry.io/platforms/native/enriching-events/ for how to
 * further work with events, and https://develop.sentry.dev/sdk/event-payloads/
 * for a detailed overview of the possible properties of an Event.
 */
SENTRY_API sentry_value_t sentry_value_new_event(void);

/**
 * Creates a new Message Event value.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/message/
 *
 * `logger` can be NULL to omit the logger value.
 */
SENTRY_API sentry_value_t sentry_value_new_message_event(
    sentry_level_t level, const char *logger, const char *text);
SENTRY_API sentry_value_t sentry_value_new_message_event_n(sentry_level_t level,
    const char *logger, size_t logger_len, const char *text, size_t text_len);

/**
 * Creates a new Breadcrumb with a specific type and message.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/breadcrumbs/
 *
 * Either parameter can be NULL in which case no such attributes are created.
 */
SENTRY_API sentry_value_t sentry_value_new_breadcrumb(
    const char *type, const char *message);
SENTRY_API sentry_value_t sentry_value_new_breadcrumb_n(
    const char *type, size_t type_len, const char *message, size_t message_len);

/**
 * Creates a new Exception value.
 *
 * This is intended for capturing language-level exception, such as from a
 * try-catch block. `type` and `value` here refer to the exception class and
 * a possible description.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/exception/
 *
 * The returned value needs to be attached to an event via
 * `sentry_event_add_exception`.
 */
SENTRY_EXPERIMENTAL_API sentry_value_t sentry_value_new_exception(
    const char *type, const char *value);
SENTRY_EXPERIMENTAL_API sentry_value_t sentry_value_new_exception_n(
    const char *type, size_t type_len, const char *value, size_t value_len);

/**
 * Creates a new Thread value.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/threads/
 *
 * The returned value needs to be attached to an event via
 * `sentry_event_add_thread`.
 *
 * `name` can be NULL.
 */
SENTRY_EXPERIMENTAL_API sentry_value_t sentry_value_new_thread(
    uint64_t id, const char *name);
SENTRY_EXPERIMENTAL_API sentry_value_t sentry_value_new_thread_n(
    uint64_t id, const char *name, size_t name_len);

/**
 * Creates a new Stack Trace conforming to the Stack Trace Interface.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/stacktrace/
 *
 * The returned object must be attached to either an exception or thread
 * object.
 *
 * If `ips` is NULL, the current stack trace is captured. Otherwise, `len`
 * stack trace instruction pointers are attached to the event.
 */
SENTRY_EXPERIMENTAL_API sentry_value_t sentry_value_new_stacktrace(
    void **ips, size_t len);

/**
 * Sets the Stack Trace conforming to the Stack Trace Interface in a value.
 *
 * The value argument must be either an exception or a thread object.
 *
 * If `ips` is NULL, the current stack trace is captured. Otherwise, `len` stack
 * trace instruction pointers are attached to the event.
 */
SENTRY_EXPERIMENTAL_API void sentry_value_set_stacktrace(
    sentry_value_t value, void **ips, size_t len);

/**
 * Adds an Exception to an Event value.
 *
 * This takes ownership of the `exception`.
 */
SENTRY_EXPERIMENTAL_API void sentry_event_add_exception(
    sentry_value_t event, sentry_value_t exception);

/**
 * Adds a Thread to an Event value.
 *
 * This takes ownership of the `thread`.
 */
SENTRY_EXPERIMENTAL_API void sentry_event_add_thread(
    sentry_value_t event, sentry_value_t thread);

/* -- Experimental APIs -- */

/**
 * Serialize a sentry value to msgpack.
 *
 * The string is freshly allocated and must be freed with
 * `sentry_free`. Since msgpack is not zero terminated,
 * the size is written to the `size_out` parameter.
 */
SENTRY_EXPERIMENTAL_API char *sentry_value_to_msgpack(
    sentry_value_t value, size_t *size_out);

/**
 * Adds a stack trace to an event.
 *
 * The stack trace is added as part of a new thread object.
 *
 * If `ips` is NULL, the current stack trace is captured. Otherwise, `len`
 * stack trace instruction pointers are attached to the event.
 */
SENTRY_DEPRECATED(
    "Use `sentry_value_new_stacktrace` in combination with "
    "`sentry_value_new_thread` and `sentry_event_add_thread` instead")
SENTRY_EXPERIMENTAL_API void sentry_event_value_add_stacktrace(
    sentry_value_t event, void **ips, size_t len);

/**
 * This represents the OS-dependent user context in the case of a crash and can
 * be used to manually capture a crash.
 */
typedef struct sentry_ucontext_s {
#ifdef _WIN32
    EXCEPTION_POINTERS exception_ptrs;
#elif defined(SENTRY_PLATFORM_PS)
    int data;
#else
    int signum;
    siginfo_t *siginfo;
    ucontext_t *user_context;
#endif
} sentry_ucontext_t;

/**
 * Unwinds the stack from the given address.
 *
 * If the address is given in `addr `, the stack is unwound from there.
 * Otherwise (NULL is passed), the current instruction pointer is used as
 * the start address.
 * Unwinding with a given `addr` is not supported on all platforms.
 *
 * The stack trace in the form of instruction-addresses is written to the
 * caller allocated `stacktrace_out`, with up to `max_len` frames being written.
 * The actual number of unwound stack frames is returned.
 */
SENTRY_EXPERIMENTAL_API size_t sentry_unwind_stack(
    void *addr, void **stacktrace_out, size_t max_len);

/**
 * Unwinds the stack from the given context.
 *
 * The caller is responsible for constructing an appropriate
 * `sentry_ucontext_t`. Unwinding from a user context is not supported on all
 * platforms.
 *
 * The stack trace in the form of instruction-addresses is written to the
 * caller allocated `stacktrace_out`, with up to `max_len` frames being written.
 * The actual number of unwound stack frames is returned.
 */
SENTRY_EXPERIMENTAL_API size_t sentry_unwind_stack_from_ucontext(
    const sentry_ucontext_t *uctx, void **stacktrace_out, size_t max_len);

/**
 * A UUID
 */
typedef struct sentry_uuid_s {
    char bytes[16];
} sentry_uuid_t;

/**
 * Creates the nil UUID.
 */
SENTRY_API sentry_uuid_t sentry_uuid_nil(void);

/**
 * Creates a new UUID4.
 */
SENTRY_API sentry_uuid_t sentry_uuid_new_v4(void);

/**
 * Parses a UUID from a string.
 */
SENTRY_API sentry_uuid_t sentry_uuid_from_string(const char *str);
SENTRY_API sentry_uuid_t sentry_uuid_from_string_n(
    const char *str, size_t str_len);

/**
 * Creates a UUID from bytes.
 */
SENTRY_API sentry_uuid_t sentry_uuid_from_bytes(const char bytes[16]);

/**
 * Checks if the UUID is nil.
 */
SENTRY_API int sentry_uuid_is_nil(const sentry_uuid_t *uuid);

/**
 * Returns the bytes of the UUID.
 */
SENTRY_API void sentry_uuid_as_bytes(const sentry_uuid_t *uuid, char bytes[16]);

/**
 * Formats the UUID into a string buffer.
 */
SENTRY_API void sentry_uuid_as_string(const sentry_uuid_t *uuid, char str[37]);

/**
 * A Sentry Envelope.
 *
 * The Envelope is an abstract type that represents a payload being sent to
 * sentry. It can contain one or more items, typically an Event.
 * See https://develop.sentry.dev/sdk/envelopes/
 */
struct sentry_envelope_s;
typedef struct sentry_envelope_s sentry_envelope_t;

/**
 * Frees an envelope.
 */
SENTRY_API void sentry_envelope_free(sentry_envelope_t *envelope);

/**
 * Given an Envelope, returns the header if present.
 *
 * This returns a borrowed value to the headers in the Envelope.
 */
SENTRY_API sentry_value_t sentry_envelope_get_header(
    const sentry_envelope_t *envelope, const char *key);
SENTRY_API sentry_value_t sentry_envelope_get_header_n(
    const sentry_envelope_t *envelope, const char *key, size_t key_len);

/**
 * Given an Envelope, returns the embedded Event if there is one.
 *
 * This returns a borrowed value to the Event in the Envelope.
 */
SENTRY_API sentry_value_t sentry_envelope_get_event(
    const sentry_envelope_t *envelope);

/**
 * Given an Envelope, returns the embedded Transaction if there is one.
 *
 * This returns a borrowed value to the Transaction in the Envelope.
 */
SENTRY_EXPERIMENTAL_API sentry_value_t sentry_envelope_get_transaction(
    const sentry_envelope_t *envelope);

/**
 * Serializes the envelope.
 *
 * The return value needs to be freed with `sentry_free`.
 */
SENTRY_API char *sentry_envelope_serialize(
    const sentry_envelope_t *envelope, size_t *size_out);

/**
 * Serializes the envelope into a file.
 *
 * `path` is assumed to be in a platform-specific filesystem path encoding.
 *
 * Returns 0 on success.
 */
SENTRY_API int sentry_envelope_write_to_file(
    const sentry_envelope_t *envelope, const char *path);
SENTRY_API int sentry_envelope_write_to_file_n(
    const sentry_envelope_t *envelope, const char *path, size_t path_len);

/**
 * De-serializes an envelope.
 *
 * The return value needs to be freed with sentry_envelope_free().
 *
 * Returns NULL on failure.
 */
SENTRY_API sentry_envelope_t *sentry_envelope_deserialize(
    const char *buf, size_t buf_len);

/**
 * De-serializes an envelope from a file.
 *
 * `path` is assumed to be in a platform-specific filesystem path encoding.
 *
 * API Users on windows are encouraged to use `sentry_envelope_read_from_filew`
 * instead.
 */
SENTRY_API sentry_envelope_t *sentry_envelope_read_from_file(const char *path);
SENTRY_API sentry_envelope_t *sentry_envelope_read_from_file_n(
    const char *path, size_t path_len);

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Wide char versions of `sentry_envelope_read_from_file` and
 * `sentry_envelope_read_from_file_n`.
 */
SENTRY_API sentry_envelope_t *sentry_envelope_read_from_filew(
    const wchar_t *path);
SENTRY_API sentry_envelope_t *sentry_envelope_read_from_filew_n(
    const wchar_t *path, size_t path_len);
#endif

/**
 * Submits an envelope, first checking for consent.
 */
SENTRY_API void sentry_capture_envelope(sentry_envelope_t *envelope);

/**
 * The Sentry Client Options.
 *
 * See https://docs.sentry.io/platforms/native/configuration/
 */
struct sentry_options_s;
typedef struct sentry_options_s sentry_options_t;

/**
 * This represents an interface for user-defined transports.
 *
 * Transports are responsible for sending envelopes to sentry and are the last
 * step in the event pipeline.
 *
 * Envelopes will be submitted to the transport in a _fire and forget_ fashion,
 * and the transport must send those envelopes _in order_.
 *
 * Transport has the following hooks, all of which
 * take the user-provided `state` as the last parameter. The transport state
 * needs to be set with `sentry_transport_set_state` and typically holds handles
 * and other information that can be reused across requests.
 *
 * * `send_func`: This function will take ownership of an envelope and is
 *   responsible for freeing it via `sentry_envelope_free`.
 * * `startup_func`: This hook will be called by sentry inside `sentry_init`
 *   and instructs the transport to initialize itself. Failures will bubble up
 *   to `sentry_init`.
 * * `flush_func`: Instructs the transport to flush its queue.
 *   This hook receives a millisecond-resolution `timeout` parameter and should
 *   return `0` if the transport queue is flushed within the timeout.
 * * `shutdown_func`: Instructs the transport to flush its queue and shut down.
 *   This hook receives a millisecond-resolution `timeout` parameter and should
 *   return `0` if the transport is flushed and shut down successfully.
 *   In case of a non-zero return value, sentry will log an error but continue
 * with freeing the transport.
 * * `free_func`: Frees the transport `state`. This hook might be called even
 *   though `shutdown_func` returned a failure code previously.
 *
 * The transport interface might be extended in the future with hooks to flush
 * its internal queue without shutting down and to dump its internal queue to
 * disk in case of a hard crash.
 */
struct sentry_transport_s;
typedef struct sentry_transport_s sentry_transport_t;

/**
 * Creates transport with an initial `send_func`.
 */
SENTRY_API sentry_transport_t *sentry_transport_new(
    void (*send_func)(sentry_envelope_t *envelope, void *state));

/**
 * Sets the transport `state`.
 *
 * If the state is owned by the transport and needs to be freed, use
 * `sentry_transport_set_free_func` to set an appropriate hook.
 */
SENTRY_API void sentry_transport_set_state(
    sentry_transport_t *transport, void *state);

/**
 * Sets the transport hook to free the transport `state`.
 */
SENTRY_API void sentry_transport_set_free_func(
    sentry_transport_t *transport, void (*free_func)(void *state));

/**
 * Sets the transport startup hook.
 *
 * This hook is called from within `sentry_init` and will get a reference to the
 * options which can be used to initialize transport internal state.
 * It should return `0` on success. A failure will bubble up to `sentry_init`.
 */
SENTRY_API void sentry_transport_set_startup_func(sentry_transport_t *transport,
    int (*startup_func)(const sentry_options_t *options, void *state));

/**
 * Sets the transport flush hook.
 *
 * This hook will receive a millisecond-resolution timeout.
 * It should return `0` if all the pending envelopes are
 * sent within the timeout, or `1` if the timeout is hit.
 */
SENTRY_API void sentry_transport_set_flush_func(sentry_transport_t *transport,
    int (*flush_func)(uint64_t timeout, void *state));

/**
 * Sets the transport shutdown hook.
 *
 * This hook will receive a millisecond-resolution timeout.
 * It should return `0` on success in case all the pending envelopes have been
 * sent within the timeout, or `1` if the timeout was hit.
 */
SENTRY_API void sentry_transport_set_shutdown_func(
    sentry_transport_t *transport,
    int (*shutdown_func)(uint64_t timeout, void *state));

/**
 * Retries sending all pending envelopes in the transport's retry queue,
 * e.g. when coming back online. Only applicable for HTTP transports.
 *
 * Note: The SDK automatically retries failed envelopes on next application
 * startup. This function allows manual triggering of pending retries at
 * runtime. Each envelope is retried up to 6 times. If all attempts are
 * exhausted during intermittent connectivity, events will be discarded
 * (or moved to cache if enabled via sentry_options_set_cache_keep).
 *
 * Warning: This function has no rate limiting - it will immediately
 * attempt to send all pending envelopes. Calling this repeatedly during
 * extended network outages may exhaust retry attempts that might have
 * succeeded with the SDK's built-in exponential backoff.
 */
SENTRY_EXPERIMENTAL_API void sentry_transport_retry(
    sentry_transport_t *transport);

/**
 * Generic way to free transport.
 */
SENTRY_API void sentry_transport_free(sentry_transport_t *transport);

/**
 * Create a new function transport.
 *
 * It is a convenience function that works with a borrowed `data`, and will
 * automatically free the envelope, so the user-provided function does not need
 * to do that.
 */
SENTRY_DEPRECATED("Use `sentry_transport_new` instead")
SENTRY_API sentry_transport_t *sentry_new_function_transport(
    void (*func)(const sentry_envelope_t *envelope, void *data), void *data);

/**
 * This represents an interface for user-defined backends.
 *
 * Backends are responsible for handling crashes. They are maintained at runtime
 * via various life-cycle hooks from the sentry-core.
 *
 * At this point none of those interfaces are exposed in the API, including
 * creation and destruction. The main use-case of the backend in the API at this
 * point is to disable it via `sentry_options_set_backend` at runtime before it
 * is initialized.
 */
struct sentry_backend_s;
typedef struct sentry_backend_s sentry_backend_t;

/* -- Options APIs -- */

/**
 * The state of user consent.
 */
typedef enum {
    SENTRY_USER_CONSENT_UNKNOWN = -1,
    SENTRY_USER_CONSENT_GIVEN = 1,
    SENTRY_USER_CONSENT_REVOKED = 0,
} sentry_user_consent_t;

/**
 * The crash handler strategy.
 */
typedef enum {
    SENTRY_HANDLER_STRATEGY_DEFAULT = 0,
    SENTRY_HANDLER_STRATEGY_CHAIN_AT_START = 1,
} sentry_handler_strategy_t;

/**
 * The minidump capture mode for the native backend.
 *
 * This controls how much memory is captured in crash minidumps.
 */
typedef enum {
    /**
     * Capture only stack memory (~100KB-1MB).
     * Fastest and smallest. Suitable for production environments with
     * high crash volumes. Provides basic crash analysis.
     */
    SENTRY_MINIDUMP_MODE_STACK_ONLY = 0,

    /**
     * Capture stack + heap around crash site (~5-10MB).
     * Balanced mode providing good crash analysis without excessive overhead.
     * This is the default and recommended for most applications.
     */
    SENTRY_MINIDUMP_MODE_SMART = 1,

    /**
     * Capture full process memory (10s-100s MB).
     * Most comprehensive debugging information but slowest to generate
     * and upload. Best for development/staging environments or critical
     * crash investigations.
     */
    SENTRY_MINIDUMP_MODE_FULL = 2,
} sentry_minidump_mode_t;

/**
 * Crash reporting mode for the native backend.
 * Controls what data is collected and sent on crash.
 */
typedef enum {
    /**
     * Minidump only (legacy behavior).
     * Write and send minidump for server-side symbolication.
     * The server will unwind the stack and symbolicate.
     */
    SENTRY_CRASH_REPORTING_MODE_MINIDUMP = 0,

    /**
     * Native stackwalking only.
     * Walk stack client-side in crash daemon, send JSON event with
     * stacktraces and debug_meta. No minidump generated.
     * Faster upload, smaller payload, but less debugging information.
     */
    SENTRY_CRASH_REPORTING_MODE_NATIVE = 1,

    /**
     * Native stackwalking with minidump attachment (default).
     * Same as NATIVE mode, but also attaches minidump for debugging.
     * Native stacktrace is primary event, minidump is attachment only.
     * Best of both worlds: fast client-side unwinding with full minidump
     * available for deep debugging when needed.
     */
    SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP = 2,
} sentry_crash_reporting_mode_t;

/**
 * Crash upload mode for the native backend.
 * Controls whether the crashed application remains blocked while upload and
 * shutdown work finishes after crash data has been captured.
 */
typedef enum {
    /**
     * Keep the crashed application blocked until the native crash daemon
     * finishes upload and shutdown work.
     */
    SENTRY_CRASH_UPLOAD_MODE_SYNC = 0,

    /**
     * Allow the crashed application to terminate after crash data has been
     * captured. The native crash daemon continues upload and shutdown work.
     */
    SENTRY_CRASH_UPLOAD_MODE_ASYNC = 1,
} sentry_crash_upload_mode_t;

/**
 * Controls if and when envelopes are kept in the persistent cache.
 */
typedef enum {
    /** Do not keep envelopes in the persistent cache. */
    SENTRY_CACHE_KEEP_NONE = 0,

    /**
     * Envelopes that cannot be uploaded immediately are written to a `cache/`
     * subdirectory within the database directory. This includes network
     * failures and envelopes captured while user consent is revoked.
     */
    SENTRY_CACHE_KEEP_OFFLINE = 1,

    /**
     * Envelopes are written to the cache regardless of the upload result. Kept
     * entries use non-retry filenames and are not picked up by the
     * retry queue.
     */
    SENTRY_CACHE_KEEP_ALWAYS = 2,
} sentry_cache_keep_t;

/**
 * Creates a new options struct.
 * Can be freed with `sentry_options_free`.
 */
SENTRY_API sentry_options_t *sentry_options_new(void);

/**
 * Deallocates previously allocated sentry options.
 */
SENTRY_API void sentry_options_free(sentry_options_t *opts);

/**
 * Sets a transport.
 */
SENTRY_API void sentry_options_set_transport(
    sentry_options_t *opts, sentry_transport_t *transport);

#ifdef SENTRY_PLATFORM_NX
/**
 * Function to start a network connection.
 * This is called on a background thread, so it must be thread-safe.
 */
SENTRY_API void sentry_options_set_network_connect_func(
    sentry_options_t *opts, void (*network_connect_func)(void));

/**
 * If false (the default), the SDK won't add PII or other sensitive data to the
 * payload. For example, a pseudo-random identifier combining device and app ID.
 */
SENTRY_API void sentry_options_set_send_default_pii(
    sentry_options_t *opts, int value);
#endif

/**
 * Type of the `before_send` callback.
 *
 * The callback takes ownership of the `event`, and should usually return that
 * same event. In case the event should be discarded, the callback needs to
 * call `sentry_value_decref` on the provided event and return a
 * `sentry_value_new_null()` instead.
 *
 * If you have set an `on_crash` callback (independent of whether it discards or
 * retains the event), `before_send` will no longer be invoked for crash-events,
 * which allows you to better distinguish between crashes and all other events
 * in client-side pre-processing.
 *
 * This function may be invoked inside a signal handler and must be safe for
 * that purpose, see https://man7.org/linux/man-pages/man7/signal-safety.7.html.
 * On Windows, it may be called from inside a `UnhandledExceptionFilter`, see
 * the documentation on SEH (structured exception handling) for more information
 * https://docs.microsoft.com/en-us/windows/win32/debug/structured-exception-handling
 *
 * Up to version 0.4.18 the `before_send` callback wasn't invoked in case the
 * event sampling discarded an event. In the current implementation the
 * `before_send` callback is invoked even if the event sampling discards the
 * event, following the cross-SDK session filter order:
 *
 * https://develop.sentry.dev/sdk/sessions/#filter-order
 *
 * On Windows, the crashpad and native backends can capture fast-fail crashes
 * which bypass SEH. Since the `before_send` callback is called by a local
 * exception-handler, it will not be invoked when such a crash happened, even
 * though a crash report will be sent.
 */
typedef sentry_value_t (*sentry_event_function_t)(
    sentry_value_t event, void *hint, void *user_data);

/**
 * Sets the `before_send` callback.
 *
 * See the `sentry_event_function_t` typedef above for more information.
 */
SENTRY_API void sentry_options_set_before_send(
    sentry_options_t *opts, sentry_event_function_t func, void *user_data);

/**
 * Type of the `on_crash` callback.
 *
 * The `on_crash` callback replaces the `before_send` callback for crash events.
 * The interface is analogous to `before_send` in that the callback takes
 * ownership of the `event ` and should usually return that same event. In case
 * the event should be discarded, the callback needs to call
 * `sentry_value_decref` on the provided event and return a
 * `sentry_value_new_null()` instead.
 *
 * Only the `inproc` backend currently fills the passed-in event with crash
 * meta-data. Since both `breakpad` and `crashpad` use minidumps to capture the
 * crash state, the passed-in event is empty when using these backends. Changes
 * to the event from inside the hooks will be passed along, but in the case of
 * the minidump backends these changes might get overwritten during server-side
 * ingestion and processing. This primarily affects the exception payloads which
 * are auto-generated from the minidump content. See
 * https://github.com/getsentry/sentry-native/issues/1147 for details.
 *
 * If you set this callback in the options, it prevents a concurrently enabled
 * `before_send` callback from being invoked in the crash case. This allows for
 * better differentiation between crashes and other events and gradual migration
 * from existing `before_send` implementations:
 *
 *  - if you have a `before_send` implementation and do not define an `on_crash`
 *    callback your application will receive both normal and crash events as
 *    before
 *  - if you have a `before_send` implementation but only want to handle normal
 *    events with it, then you can define an `on_crash` callback that returns
 *    the passed-in event and does nothing else
 *  - if you are not interested in normal events, but only want to act on
 *    crashes (within the limits mentioned below), then only define an
 *    `on_crash` callback with the option to filter (on all backends) or enrich
 *    (only inproc) the crash event
 *
 * This function may be invoked inside a signal handler and must be safe for
 * that purpose, see https://man7.org/linux/man-pages/man7/signal-safety.7.html.
 * On Windows, it may be called from inside a `UnhandledExceptionFilter`, see
 * the documentation on SEH (structured exception handling) for more information
 * https://docs.microsoft.com/en-us/windows/win32/debug/structured-exception-handling
 *
 * Platform-specific behavior:
 *
 *  - does not work with crashpad on macOS.
 *  - for breakpad on Linux the `uctx` parameter is always NULL.
 *  - on Windows, the crashpad and native backends can capture fast-fail crashes
 *    which bypass SEH. Since `on_crash` is called by a local
 *    exception-handler, it will not be invoked when such a crash happened, even
 *    though a crash report will be sent.
 */
typedef sentry_value_t (*sentry_crash_function_t)(
    const sentry_ucontext_t *uctx, sentry_value_t event, void *user_data);

/**
 * Sets the `on_crash` callback.
 *
 * See the `sentry_crash_function_t` typedef above for more information.
 */
SENTRY_API void sentry_options_set_on_crash(
    sentry_options_t *opts, sentry_crash_function_t func, void *data);

/**
 * Sets the DSN.
 */
SENTRY_API void sentry_options_set_dsn(sentry_options_t *opts, const char *dsn);
SENTRY_API void sentry_options_set_dsn_n(
    sentry_options_t *opts, const char *dsn, size_t dsn_len);

/**
 * Gets the DSN.
 */
SENTRY_API const char *sentry_options_get_dsn(const sentry_options_t *opts);

/**
 * Sets the sample rate, which should be a double between `0.0` and `1.0`.
 * Sentry will randomly discard any event that is captured using
 * `sentry_capture_event` when a sample rate < 1 is set.
 *
 * The sampling happens at the end of the event processing according to the
 * following order:
 *
 * https://develop.sentry.dev/sdk/sessions/#filter-order
 *
 * Only items 3. to 6. are currently applicable to sentry-native. This means
 * each processing step is executed even if the sampling discards the event
 * before sending it to the backend. This is particularly relevant to users of
 * the `before_send` callback.
 *
 * The above is in contrast to versions up to 0.4.18 where the sampling happened
 * at the beginning of the processing/filter sequence.
 */
SENTRY_API void sentry_options_set_sample_rate(
    sentry_options_t *opts, double sample_rate);

/**
 * Gets the sample rate.
 */
SENTRY_API double sentry_options_get_sample_rate(const sentry_options_t *opts);

/**
 * Sets the release.
 */
SENTRY_API void sentry_options_set_release(
    sentry_options_t *opts, const char *release);
SENTRY_API void sentry_options_set_release_n(
    sentry_options_t *opts, const char *release, size_t release_len);

/**
 * Gets the release.
 */
SENTRY_API const char *sentry_options_get_release(const sentry_options_t *opts);

/**
 * Sets the environment.
 */
SENTRY_API void sentry_options_set_environment(
    sentry_options_t *opts, const char *environment);
SENTRY_API void sentry_options_set_environment_n(
    sentry_options_t *opts, const char *environment, size_t environment_len);

/**
 * Gets the environment.
 */
SENTRY_API const char *sentry_options_get_environment(
    const sentry_options_t *opts);

/**
 * Sets the dist.
 */
SENTRY_API void sentry_options_set_dist(
    sentry_options_t *opts, const char *dist);
SENTRY_API void sentry_options_set_dist_n(
    sentry_options_t *opts, const char *dist, size_t dist_len);

/**
 * Gets the dist.
 */
SENTRY_API const char *sentry_options_get_dist(const sentry_options_t *opts);

/**
 * Configures the proxy.
 *
 * The given proxy has to include the full scheme,
 * e.g., `http://some.proxy/` or `socks5://some.proxy/`.
 *
 * Not every transport behaves the same way when configuring a proxy.
 * On Windows if a transport can't connect to the proxy, it will fall back on a
 * connection without a proxy. This is also true for the crashpad_handler
 * transport on macOS for a SOCKS proxy, but not for an HTTP proxy.
 * All transports that use libcurl (Linux and the Native SDK transport on macOS)
 * will honor the proxy settings and not fall back.
 */
SENTRY_API void sentry_options_set_proxy(
    sentry_options_t *opts, const char *proxy);
SENTRY_API void sentry_options_set_proxy_n(
    sentry_options_t *opts, const char *proxy, size_t proxy_len);

/**
 * Returns the configured proxy.
 */
SENTRY_API const char *sentry_options_get_proxy(const sentry_options_t *opts);

/**
 * Configures the proxy.
 *
 * The given proxy has to include the full scheme,
 * e.g., `http://some.proxy/.
 */
SENTRY_DEPRECATED("Use `sentry_options_set_proxy` instead")
SENTRY_API void sentry_options_set_http_proxy(
    sentry_options_t *opts, const char *proxy);
SENTRY_DEPRECATED("Use `sentry_options_set_proxy_n` instead")
SENTRY_API void sentry_options_set_http_proxy_n(
    sentry_options_t *opts, const char *proxy, size_t proxy_len);

/**
 * Returns the configured proxy.
 */
SENTRY_API const char *sentry_options_get_http_proxy(
    const sentry_options_t *opts);

/**
 * Configures the path to a file containing ssl certificates for
 * verification.
 */
SENTRY_API void sentry_options_set_ca_certs(
    sentry_options_t *opts, const char *path);
SENTRY_API void sentry_options_set_ca_certs_n(
    sentry_options_t *opts, const char *path, size_t path_len);

/**
 * Returns the configured path for ca certificates.
 */
SENTRY_API const char *sentry_options_get_ca_certs(
    const sentry_options_t *opts);

/**
 * Configures the name of the http transport thread.
 */
SENTRY_API void sentry_options_set_transport_thread_name(
    sentry_options_t *opts, const char *name);
SENTRY_API void sentry_options_set_transport_thread_name_n(
    sentry_options_t *opts, const char *name, size_t name_len);

/**
 * Returns the configured http transport thread name.
 */
SENTRY_API const char *sentry_options_get_transport_thread_name(
    const sentry_options_t *opts);

/*
 * Configures the name of the sentry SDK. Returns 0 on success.
 */
SENTRY_API int sentry_options_set_sdk_name(
    sentry_options_t *opts, const char *sdk_name);

/*
 * Configures the name of the sentry SDK. Returns 0 on success.
 */
SENTRY_API int sentry_options_set_sdk_name_n(
    sentry_options_t *opts, const char *sdk_name, size_t sdk_name_len);

/**
 * Returns the configured sentry SDK name. Unless overwritten, this defaults to
 * SENTRY_SDK_NAME.
 */
SENTRY_API const char *sentry_options_get_sdk_name(
    const sentry_options_t *opts);

/**
 * Returns the user agent. Unless overwritten, this defaults to
 * "SENTRY_SDK_NAME / SENTRY_SDK_VERSION".
 */
SENTRY_API const char *sentry_options_get_user_agent(
    const sentry_options_t *opts);

/**
 * Enables or disables debug printing mode. To change the log level from the
 * default DEBUG level, use `sentry_options_set_logger_level`.
 */
SENTRY_API void sentry_options_set_debug(sentry_options_t *opts, int debug);

/**
 * Returns the current value of the debug flag.
 */
SENTRY_API int sentry_options_get_debug(const sentry_options_t *opts);

/**
 * Sets the level of the logger. Has no effect if `debug` is not set to true.
 */
SENTRY_API void sentry_options_set_logger_level(
    sentry_options_t *opts, sentry_level_t level);

/**
 * Sets the number of breadcrumbs being tracked and attached to events.
 *
 * Defaults to 100.
 */
SENTRY_API void sentry_options_set_max_breadcrumbs(
    sentry_options_t *opts, size_t max_breadcrumbs);

/**
 * Gets the number of breadcrumbs being tracked and attached to events.
 */
SENTRY_API size_t sentry_options_get_max_breadcrumbs(
    const sentry_options_t *opts);

/**
 * Type of the callback for a logger function.
 */
typedef void (*sentry_logger_function_t)(
    sentry_level_t level, const char *message, va_list args, void *userdata);

/**
 * Sets the sentry-native logger function.
 *
 * Used for logging debug events when the `debug` option is set to true.
 *
 * Note: Multiple threads may invoke your `func`. If you plan to mutate any data
 * inside the `userdata` argument after initialization, you must ensure proper
 * synchronization inside the logger function.
 *
 */
SENTRY_API void sentry_options_set_logger(
    sentry_options_t *opts, sentry_logger_function_t func, void *userdata);

/**
 * Enables or disables console logging after a crash.
 *
 * When disabled, Sentry will not invoke logger callbacks after a crash
 * has been detected. This can be useful to avoid potential issues during
 * crash handling that logging might cause.
 *
 * Enabled by default, except for the breakpad backend on macOS, where
 * it defaults to off because the in-process Mach exception handler can
 * deadlock on stdio locks held by threads that were suspended at crash
 * time.
 */
SENTRY_API void sentry_options_set_logger_enabled_when_crashed(
    sentry_options_t *opts, int enabled);

/**
 * Enables or disables automatic session tracking.
 *
 * Automatic session tracking is enabled by default and is equivalent to calling
 * `sentry_start_session` after startup.
 * There can only be one running session, and the current session will always be
 * closed implicitly by `sentry_close`, when starting a new session with
 * `sentry_start_session`, or manually by calling `sentry_end_session`.
 */
SENTRY_API void sentry_options_set_auto_session_tracking(
    sentry_options_t *opts, int val);

/**
 * Returns true if automatic session tracking is enabled.
 */
SENTRY_API int sentry_options_get_auto_session_tracking(
    const sentry_options_t *opts);

/**
 * Enables or disables user consent requirements for uploads.
 *
 * This disables uploads until the user has given the consent to the SDK.
 * Consent itself is given with `sentry_user_consent_give` and
 * `sentry_user_consent_revoke`.
 *
 * When combined with `cache_keep` or `http_retry`, envelopes captured
 * while consent is revoked are written to the cache directory instead
 * of being discarded. With `http_retry` enabled, cached envelopes are
 * sent automatically once consent is given.
 */
SENTRY_API void sentry_options_set_require_user_consent(
    sentry_options_t *opts, int val);

/**
 * Returns true if user consent is required.
 */
SENTRY_API int sentry_options_get_require_user_consent(
    const sentry_options_t *opts);

/**
 * Enables or disables on-device symbolication of stack traces.
 *
 * This feature can have a performance impact and is enabled by default on
 * Android. It is usually only necessary when it is not possible to provide
 * debug information files for system libraries which are needed for serverside
 * symbolication.
 */
SENTRY_API void sentry_options_set_symbolize_stacktraces(
    sentry_options_t *opts, int val);

/**
 * Returns true if on-device symbolication of stack traces is enabled.
 */
SENTRY_API int sentry_options_get_symbolize_stacktraces(
    const sentry_options_t *opts);

/**
 * Configures if and when envelopes are kept in a persistent cache.
 *
 * Kept envelopes are written to a `cache/` subdirectory within the database
 * directory. The cache is cleared on startup based on the cache_max_items,
 * cache_max_size, and cache_max_age options.
 *
 * When combined with `sentry_options_set_require_user_consent`, envelopes
 * captured while consent is revoked are also written to the cache. With
 * `http_retry` enabled, they are sent once consent is given.
 *
 * Only applicable for HTTP transports.
 *
 * `SENTRY_CACHE_KEEP_NONE` by default.
 */
SENTRY_API void sentry_options_set_cache_keep(sentry_options_t *opts, int mode);

/**
 * Sets the maximum number of items in the cache directory.
 * On startup, cached entries are removed from oldest to newest until the
 * directory contains at most the specified number of items.
 *
 * Defaults to 30.
 */
SENTRY_API void sentry_options_set_cache_max_items(
    sentry_options_t *opts, size_t items);

/**
 * Sets the maximum size (in bytes) for the cache directory.
 * On startup, cached entries are removed from oldest to newest until the
 * directory size is within the max size limit.
 *
 * Defaults to 0 (no max size).
 */
SENTRY_API void sentry_options_set_cache_max_size(
    sentry_options_t *opts, size_t bytes);

/**
 * Sets the maximum age (in seconds) for cache entries in the cache directory.
 * On startup, cached entries exceeding the max age limit are removed.
 *
 * Defaults to 0 (no max age).
 */
SENTRY_API void sentry_options_set_cache_max_age(
    sentry_options_t *opts, time_t seconds);

/**
 * Gets the caching mode for crash reports.
 */
SENTRY_API int sentry_options_get_cache_keep(const sentry_options_t *opts);

/**
 * Adds a new attachment to be sent along.
 *
 * `path` is assumed to be in a platform-specific filesystem path encoding.
 * API Users on windows are encouraged to use `sentry_options_add_attachmentw`
 * instead.
 *
 * See the NOTE on attachments above for restrictions of this API.
 */
SENTRY_API void sentry_options_add_attachment(
    sentry_options_t *opts, const char *path);
SENTRY_API void sentry_options_add_attachment_n(
    sentry_options_t *opts, const char *path, size_t path_len);

/**
 * Adds a new view hierarchy attachment to be sent along.
 *
 * The primary use-case is for downstream SDKs (like sentry-godot). The
 * view-hierarchy.json file should follow the representation defined in RFC#33
 * https://github.com/getsentry/rfcs/blob/main/text/0033-view-hierarchy.md
 *
 * `path` is assumed to be in platform-specific filesystem path encoding.
 * API Users on windows are encouraged to use
 * `sentry_options_add_view_hierarchyw` instead.
 */
SENTRY_DEPRECATED("Use `sentry_attach_*` with `sentry_attachment_set_type` and "
                  "`SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY` instead")
SENTRY_API void sentry_options_add_view_hierarchy(
    sentry_options_t *opts, const char *path);
SENTRY_DEPRECATED("Use `sentry_attach_*` with `sentry_attachment_set_type` and "
                  "`SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY` instead")
SENTRY_API void sentry_options_add_view_hierarchy_n(
    sentry_options_t *opts, const char *path, size_t path_len);

/**
 * Enables or disables attaching screenshots to fatal error events. Disabled by
 * default.
 *
 * This feature is currently supported by all backends on Windows. The
 * `crashpad` and `native` backends capture screenshots from an out-of-process
 * handler. Only the `crashpad` and `native` backends can capture screenshots of
 * fast-fail crashes that bypass SEH (structured exception handling).
 *
 * To decide per-event whether a screenshot should be captured, set a
 * `before_screenshot` callback via `sentry_options_set_before_screenshot`.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_attach_screenshot(
    sentry_options_t *opts, int val);

/**
 * Type of the `before_screenshot` callback.
 *
 * The callback is invoked before a screenshot is captured for an event. It
 * receives the event (without ownership) and should return non-zero to capture
 * the screenshot, or zero to skip it. The `attach_screenshot` option must be
 * enabled for screenshots to be considered at all.
 *
 * Capturing screenshots is only supported on Windows, so the callback is
 * only invoked there. The callback is not supported by the `crashpad`
 * backend, which captures screenshots from its out-of-process handler.
 *
 * The callback may be called from inside an `UnhandledExceptionFilter`, see
 * the documentation on SEH (structured exception handling) for more
 * information
 * https://docs.microsoft.com/en-us/windows/win32/debug/structured-exception-handling
 */
typedef int (*sentry_before_screenshot_function_t)(
    sentry_value_t event, void *user_data);

/**
 * Sets the `before_screenshot` callback.
 *
 * See the `sentry_before_screenshot_function_t` typedef above for more
 * information.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_before_screenshot(
    sentry_options_t *opts, sentry_before_screenshot_function_t func,
    void *user_data);

/**
 * Enables capturing a short retroactive video replay on crash. Currently only
 * supported on Xbox via the OS-managed game recording ring. The replay is
 * attached to the crash envelope as `session-replay.mp4`.
 *
 * Set the duration via `sentry_options_set_session_replay_duration`
 * (default 5000 ms). Disabled by default. Must be set before `sentry_init`.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_attach_session_replay(
    sentry_options_t *opts, int val);

/**
 * Sets the requested duration of the retroactive session replay in
 * milliseconds.
 *
 * The resulting replay can be shorter than the requested duration if it hasn't
 * accumulated enough buffered frames yet. Defaults to 5000 ms.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_session_replay_duration(
    sentry_options_t *opts, uint32_t duration_ms);

/**
 * Sets the path to the crashpad handler if the crashpad backend is used.
 *
 * The path defaults to the `crashpad_handler`/`crashpad_handler.exe`
 * executable in the same directory as the application executable.
 *
 * Meaning if your application resides in
 *
 * "C:\path\to\your\application.exe"
 *
 * then the handler path will be set (by default) to
 *
 * "C:\path\to\your\crashpad_handler.exe"
 *
 * It is recommended that library users set an explicit handler path, depending
 * on the directory/executable structure of their app.
 *
 * `path` is assumed to be in platform-specific filesystem path encoding.
 * API Users on windows are encouraged to use `sentry_options_set_handler_pathw`
 * instead.
 */
SENTRY_API void sentry_options_set_handler_path(
    sentry_options_t *opts, const char *path);
SENTRY_API void sentry_options_set_handler_path_n(
    sentry_options_t *opts, const char *path, size_t path_len);

/**
 * Sets the path to an external crash reporter executable, which can be used to
 * display crash information to the user, collect user feedback, or perform
 * other actions when a crash occurs.
 *
 * The external crash reporter is a user-defined executable, distinct from the
 * system crash reporter, that is launched by the Native SDK upon a crash. It
 * receives the path to the crash report as its only command-line argument and
 * is responsible for submitting the crash report to Sentry. If using the Native
 * SDK, this can be done using the `sentry_capture_envelope` function.
 *
 * A well-behaved external crash reporter should delete the crash report
 * after handling it. As a safeguard, the Native SDK automatically removes
 * crash reports older than one hour on startup to prevent filling up the disk
 * with stale crash reports.
 *
 * The `path` parameter should use platform-specific filesystem encoding.
 * On Windows, API users are encouraged to use
 * `sentry_options_set_external_crash_reporter_pathw` instead.
 */
SENTRY_API void sentry_options_set_external_crash_reporter_path(
    sentry_options_t *opts, const char *path);
SENTRY_API void sentry_options_set_external_crash_reporter_path_n(
    sentry_options_t *opts, const char *path, size_t path_len);

/**
 * Sets the path to the Sentry Database Directory.
 *
 * Sentry will use this path to persist user consent, sessions, and other
 * artifacts in case of a crash. This will also be used by the crashpad backend
 * if it is configured.
 *
 * The directory is used for "cached" data, which needs to persist across
 * application restarts to ensure proper flagging of release-health sessions,
 * but might otherwise be safely purged regularly.
 *
 * It is roughly equivalent to the type of `AppData/Local` on Windows and
 * `XDG_CACHE_HOME` on Linux, and equivalent runtime directories on other
 * platforms.
 *
 * It is recommended that users set an explicit absolute path, depending
 * on their apps runtime directory. The path will be created if it does not
 * exist and will be resolved to an absolute path inside `sentry_init`. The
 * directory should not be shared with other application data/configuration, as
 * sentry-native will list and possibly delete files in that directory. An
 * example might be `$XDG_CACHE_HOME/your-app/sentry`
 *
 * If no explicit path is set, sentry-native will default to `.sentry-native` in
 * the current working directory, with no specific platform-specific handling.
 *
 * `path` is assumed to be in a platform-specific filesystem path encoding.
 * API Users on windows are encouraged to use
 * `sentry_options_set_database_pathw` instead.
 */
SENTRY_API void sentry_options_set_database_path(
    sentry_options_t *opts, const char *path);
SENTRY_API void sentry_options_set_database_path_n(
    sentry_options_t *opts, const char *path, size_t path_len);

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Wide char version of `sentry_options_add_attachment`.
 */
SENTRY_API void sentry_options_add_attachmentw(
    sentry_options_t *opts, const wchar_t *path);
SENTRY_API void sentry_options_add_attachmentw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len);

/**
 * Wide char version of `sentry_options_add_view_hierarchy`.
 */
SENTRY_DEPRECATED("Use `sentry_attach_*` with `sentry_attachment_set_type` and "
                  "`SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY` instead")
SENTRY_API void sentry_options_add_view_hierarchyw(
    sentry_options_t *opts, const wchar_t *path);
SENTRY_DEPRECATED("Use `sentry_attach_*` with `sentry_attachment_set_type` and "
                  "`SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY` instead")
SENTRY_API void sentry_options_add_view_hierarchyw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len);

/**
 * Wide char version of `sentry_options_set_handler_path`.
 */
SENTRY_API void sentry_options_set_handler_pathw(
    sentry_options_t *opts, const wchar_t *path);
SENTRY_API void sentry_options_set_handler_pathw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len);

/**
 * Wide char version of `sentry_options_set_external_crash_reporter_path`.
 */
SENTRY_API void sentry_options_set_external_crash_reporter_pathw(
    sentry_options_t *opts, const wchar_t *path);
SENTRY_API void sentry_options_set_external_crash_reporter_pathw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len);

/**
 * Wide char version of `sentry_options_set_database_path`.
 */
SENTRY_API void sentry_options_set_database_pathw(
    sentry_options_t *opts, const wchar_t *path);
SENTRY_API void sentry_options_set_database_pathw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len);

/**
 * Allows users to define a thread stack guarantee manually on each thread. This
 * is necessary for crash handlers to run after a thread crashed due to a
 * stack-overflow on Windows.
 *
 * By default, the Native SDK automatically sets this value for you, but in some
 * cases it might be preferable to set these values yourself for each thread you
 * manage. Ensure to disable the `SENTRY_THREAD_STACK_GUARANTEE_AUTO_INIT` CMake
 * option when building the Native SDK to have full control over each thread's
 * stack guarantee.
 *
 * The input parameter specifies a size in bytes and should be a multiple of the
 * page size. Returns `1` if the thread stack guarantee was set successfully and
 * `0` otherwise.
 *
 * Note: when we auto-assign the stack guarantee, we check against the thread's
 * stack reserve (see `SENTRY_THREAD_STACK_GUARANTEE_FACTOR` in the
 * `README.md`). This check is not applied when you call this function.
 * Note: this function depends on the SDK being initialized when doing static
 * builds or in any configuration on Xbox.
 */
SENTRY_EXPERIMENTAL_API int sentry_set_thread_stack_guarantee(
    uint32_t stack_guarantee_in_bytes);
#endif

/**
 * Enables forwarding to the system crash reporter. Disabled by default.
 *
 * This setting only has an effect when using the crashpad or native backend on
 * macOS. If enabled, the crash handler forwards crashes to the macOS system
 * crash reporter. Depending on the crash, this may impact the crash time. Even
 * if enabled, the crash handler may choose not to forward certain crashes.
 */
SENTRY_API void sentry_options_set_system_crash_reporter_enabled(
    sentry_options_t *opts, int enabled);

/**
 * Sets the minidump capture mode for the native backend.
 *
 * This controls how much memory is captured in crash minidumps.
 * See `sentry_minidump_mode_t` for available modes.
 *
 * Larger captures provide more debugging information but take longer to
 * generate and upload. For production, `SENTRY_MINIDUMP_MODE_STACK_ONLY` or
 * `SENTRY_MINIDUMP_MODE_SMART` are recommended.
 *
 * This setting only has an effect when using the `native` backend.
 * Default is `SENTRY_MINIDUMP_MODE_SMART`.
 */
SENTRY_API void sentry_options_set_minidump_mode(
    sentry_options_t *opts, sentry_minidump_mode_t mode);

/**
 * Sets the crash reporting mode for the native backend.
 *
 * This controls how crash data is collected and what is sent to Sentry:
 * - MINIDUMP: Traditional minidump-only mode (server-side unwinding)
 * - NATIVE: Client-side stack unwinding, JSON event with stacktraces
 * - NATIVE_WITH_MINIDUMP: Both native stacktrace and minidump attachment
 *
 * See `sentry_crash_reporting_mode_t` for detailed mode descriptions.
 *
 * This setting only has an effect when using the `native` backend.
 * Default is `SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP`.
 */
SENTRY_API void sentry_options_set_crash_reporting_mode(
    sentry_options_t *opts, sentry_crash_reporting_mode_t mode);

/**
 * Gets the crash reporting mode for the native backend.
 */
SENTRY_API sentry_crash_reporting_mode_t
sentry_options_get_crash_reporting_mode(const sentry_options_t *opts);

/**
 * Sets the crash upload mode for the native backend.
 *
 * This setting controls what happens after crash data has been captured. In
 * sync mode, the crashed application remains blocked while the native crash
 * daemon finishes upload and shutdown work. In async mode, the crashed
 * application can terminate after crash data has been captured while the daemon
 * continues upload and shutdown work.
 *
 * This setting only has an effect when using the `native` backend.
 * Default is `SENTRY_CRASH_UPLOAD_MODE_SYNC`.
 */
SENTRY_API void sentry_options_set_crash_upload_mode(
    sentry_options_t *opts, sentry_crash_upload_mode_t mode);

/**
 * Gets the crash upload mode for the native backend.
 */
SENTRY_API sentry_crash_upload_mode_t sentry_options_get_crash_upload_mode(
    const sentry_options_t *opts);

/**
 * Enables a wait for the crash report upload to be finished before shutting
 * down. This is disabled by default.
 *
 * This setting only has an effect when using the `crashpad` backend on Linux,
 * Windows and macOS.
 */
SENTRY_API void sentry_options_set_crashpad_wait_for_upload(
    sentry_options_t *opts, int wait_for_upload);

/**
 * Limits stack capture to stack pointer for the Crashpad backend.
 * When enabled, Crashpad will use the current stack pointer as the upper bound
 * of the stack capture range, once validated to be within TEB
 * StackLimit/StackBase values. This reduces the capture range compared to
 * using the full TEB-derived stack region.
 *
 * This is useful when running under Wine/Proton where the TEB values may
 * be incorrect, leading to excessively large stack captures. This is
 * disabled by default.
 *
 * This setting only has an effect when using the `crashpad` backend on Windows.
 */
SENTRY_API void sentry_options_set_crashpad_limit_stack_capture_to_sp(
    sentry_options_t *opts, int enabled);

/**
 * Sets the maximum time (in milliseconds) to wait for the asynchronous
 * tasks to end on shutdown before attempting a forced termination.
 */
SENTRY_API void sentry_options_set_shutdown_timeout(
    sentry_options_t *opts, uint64_t shutdown_timeout);

/**
 * Gets the maximum time (in milliseconds) to wait for the asynchronous tasks to
 * end on shutdown before attempting a forced termination.
 */
SENTRY_API uint64_t sentry_options_get_shutdown_timeout(sentry_options_t *opts);

/**
 * Sets the timeout (in milliseconds) for HTTP transfer operations.
 *
 * On curl, this limits the total time an HTTP request is allowed to take. On
 * WinHTTP, this is applied to send and receive operations, which WinHTTP
 * applies to individual packets.
 *
 * This setting applies to the SDK-managed HTTP transports. It is not supported
 * by Crashpad's crash report upload.
 *
 * The default value is 0, which disables the transfer timeout.
 */
SENTRY_API void sentry_options_set_transfer_timeout(
    sentry_options_t *opts, uint64_t transfer_timeout);

/**
 * Gets the timeout (in milliseconds) for HTTP transfer operations.
 */
SENTRY_API uint64_t sentry_options_get_transfer_timeout(sentry_options_t *opts);

/**
 * Sets a user-defined backend.
 *
 * Since creation and destruction of backends is not exposed in the API, this
 * can only be used to set the backend to `NULL`, which disables the backend in
 * the initialization.
 */
SENTRY_API void sentry_options_set_backend(
    sentry_options_t *opts, sentry_backend_t *backend);

/* -- Global/Scope APIs -- */

/**
 * Initializes the Sentry SDK with the specified options.
 *
 * This takes ownership of the options. After the options have been set,
 * they cannot be modified anymore.
 * Depending on the configured transport and backend, this function might not be
 * fully thread-safe.
 * Returns 0 on success.
 */
SENTRY_API int sentry_init(sentry_options_t *options);

/**
 * Instructs the transport to flush its send-queue.
 *
 * The `timeout` parameter is in milliseconds.
 *
 * Returns 0 on success, or a non-zero return value in case the timeout is hit.
 *
 * Note that this function will block the thread it was called from until the
 * sentry background worker has finished its work, or it timed out, whichever
 * comes first.
 */
SENTRY_API int sentry_flush(uint64_t timeout);

/**
 * Shuts down the sentry client and forces transports to flush out.
 *
 * Returns the number of envelopes that have been dumped.
 *
 * Note that this does not uninstall any crash handler installed by our
 * backends, which will still process crashes after `sentry_close()`, except
 * when using `crashpad` on Linux or the `inproc` backend. The Android
 * preload mode of `inproc` is a special case: a lightweight signal-chain
 * placeholder may remain installed across `sentry_close()` to preserve
 * ordering relative to the managed runtime until a later re-init.
 *
 * Further note that this function will block the thread it was called from
 * until the sentry background worker has finished its work, or it timed out,
 * whichever comes first.
 */
SENTRY_API int sentry_close(void);

/**
 * Shuts down the sentry client and forces transports to flush out.
 *
 * Returns the number of envelopes that have been dumped.
 */
SENTRY_DEPRECATED("Use `sentry_close` instead")
SENTRY_API int sentry_shutdown(void);

/**
 * This will lazily load and cache a list of all the loaded libraries.
 *
 * Returns a new reference to an immutable, frozen list.
 * The reference must be released with `sentry_value_decref`.
 */
SENTRY_EXPERIMENTAL_API sentry_value_t sentry_get_modules_list(void);

/**
 * Clears the internal module cache.
 *
 * For performance reasons, sentry will cache the list of loaded libraries when
 * capturing events. This cache can get out-of-date when loading or unloading
 * libraries at runtime. It is therefore recommended to call
 * `sentry_clear_modulecache` when doing so to make sure that the next call to
 * `sentry_capture_event` will have an up-to-date module list.
 */
SENTRY_EXPERIMENTAL_API void sentry_clear_modulecache(void);

/**
 * Re-initializes the Sentry backend.
 *
 * This is needed if a third-party library overrides the previously installed
 * signal handler. Calling this function can be potentially dangerous and should
 * only be done when necessary.
 *
 * Returns 0 on success.
 */
SENTRY_EXPERIMENTAL_API int sentry_reinstall_backend(void);

/**
 * Gives user consent.
 *
 * Schedules a retry of any envelopes cached while consent was revoked,
 * provided that `http_retry` is enabled.
 */
SENTRY_API void sentry_user_consent_give(void);

/**
 * Revokes user consent.
 */
SENTRY_API void sentry_user_consent_revoke(void);

/**
 * Resets the user consent (back to unknown).
 */
SENTRY_API void sentry_user_consent_reset(void);

/**
 * Checks the current state of user consent.
 */
SENTRY_API sentry_user_consent_t sentry_user_consent_get(void);

/**
 * Checks whether user consent is required.
 *
 * This returns the value that was configured via
 * `sentry_options_set_require_user_consent` during initialization.
 *
 * Returns 1 if user consent is required, 0 otherwise.
 */
SENTRY_API int sentry_user_consent_is_required(void);

/**
 * A sentry Scope.
 *
 * See https://develop.sentry.dev/sdk/telemetry/scopes/
 */
struct sentry_scope_s;
typedef struct sentry_scope_s sentry_scope_t;

/**
 * Creates a local scope.
 */
SENTRY_API sentry_scope_t *sentry_local_scope_new(void);

/**
 * Sends a sentry event.
 *
 * If returns a nil UUID if the event being passed in is a transaction, and the
 * transaction will not be sent nor consumed. `sentry_transaction_finish` should
 * be used to send transactions.
 */
SENTRY_API sentry_uuid_t sentry_capture_event(sentry_value_t event);

/**
 * Sends a sentry event with a local scope.
 *
 * Takes ownership of `scope`.
 */
SENTRY_API sentry_uuid_t sentry_capture_event_with_scope(
    sentry_value_t event, sentry_scope_t *scope);

/**
 * Allows capturing independently created minidumps.
 *
 * This generates a fatal error event, includes the scope and attachments.
 * If a before-send hook doesn't drop the event, the minidump is attached
 * and the event is sent.
 *
 * Returns a nil `UUID` if capturing the minidump failed and the event-id
 * otherwise. Uploads can fail because capturing is asynchronous, so a non-nil
 * `UUID` is not a delivery guarantee. However, if the minidump is successfully
 * delivered, the ID is guaranteed to be the same as the event in the Sentry UI.
 *
 * Note: You don't need this function if you rely on Sentry to create the
 * minidump. This is useful when you have a minidump captured through a
 * different mechanism, and you want Sentry to ingest it.
 */
SENTRY_API sentry_uuid_t sentry_capture_minidump(const char *path);
SENTRY_API sentry_uuid_t sentry_capture_minidump_n(
    const char *path, size_t path_len);

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Wide char versions of `sentry_capture_minidump` and
 * `sentry_capture_minidump_n`.
 */
SENTRY_API sentry_uuid_t sentry_capture_minidumpw(const wchar_t *path);
SENTRY_API sentry_uuid_t sentry_capture_minidumpw_n(
    const wchar_t *path, size_t path_len);
#endif

/**
 * Captures a system-native exception that you retrieve when you manually handle
 * `POSIX` signals or `SEH` exceptions and want to keep using that handling
 * instead of the top-level handlers in our backends. The exception is still
 * processed as a sentry event inside the SDK, including applying scope metadata
 * and invoking hooks.
 *
 * The passed in `sentry_ucontext_t` must be filled with the OS-specific
 * exception data (as specified in the struct definition) that you retrieve
 * from your handler.
 *
 * This is safe to be called from a crashing thread and may not return.
 *
 * Note: The `crashpad` client currently supports this only on Windows. `inproc`
 *       and `breakpad` support it on all platforms (on macOS, the `uctx`
 *       argument is ignored when using the `breakpad` backend).
 */
SENTRY_EXPERIMENTAL_API void sentry_handle_exception(
    const sentry_ucontext_t *uctx);

/**
 * Type of the `before_breadcrumb` callback.
 *
 * The callback takes ownership of the `breadcrumb` and should usually return
 * that same breadcrumb. In case the breadcrumb should be discarded, the
 * callback needs to call `sentry_value_decref` on the provided breadcrumb and
 * return a `sentry_value_new_null()` instead.
 *
 * The callback may also modify the breadcrumb and return it.
 */
typedef sentry_value_t (*sentry_before_breadcrumb_function_t)(
    sentry_value_t breadcrumb, void *user_data);

/**
 * Sets the `before_breadcrumb` callback.
 *
 * See the `sentry_before_breadcrumb_function_t` typedef above for more
 * information.
 */
SENTRY_API void sentry_options_set_before_breadcrumb(sentry_options_t *opts,
    sentry_before_breadcrumb_function_t func, void *data);

/**
 * Adds the breadcrumb to be sent in case of an event.
 */
SENTRY_API void sentry_add_breadcrumb(sentry_value_t breadcrumb);
SENTRY_API void sentry_scope_add_breadcrumb(
    sentry_scope_t *scope, sentry_value_t breadcrumb);

/**
 * Sets the specified user.
 */
SENTRY_API void sentry_set_user(sentry_value_t user);
SENTRY_API void sentry_scope_set_user(
    sentry_scope_t *scope, sentry_value_t user);

/**
 * Removes a user.
 */
SENTRY_API void sentry_remove_user(void);

/**
 * Sets the release after the SDK has been initialized. To apply the new release
 * to sessions, start a new session after calling this function.
 */
SENTRY_API void sentry_set_release(const char *release);
SENTRY_API void sentry_set_release_n(const char *release, size_t release_len);

/**
 * Sets the environment after the SDK has been initialized. To apply the new
 * environment to sessions, start a new session after calling this function.
 */
SENTRY_API void sentry_set_environment(const char *environment);
SENTRY_API void sentry_set_environment_n(
    const char *environment, size_t environment_len);

/**
 * Sets a tag.
 */
SENTRY_API void sentry_set_tag(const char *key, const char *value);
SENTRY_API void sentry_set_tag_n(
    const char *key, size_t key_len, const char *value, size_t value_len);
SENTRY_API void sentry_scope_set_tag(
    sentry_scope_t *scope, const char *key, const char *value);
SENTRY_API void sentry_scope_set_tag_n(sentry_scope_t *scope, const char *key,
    size_t key_len, const char *value, size_t value_len);

/**
 * Removes the tag with the specified key.
 */
SENTRY_API void sentry_remove_tag(const char *key);
SENTRY_API void sentry_remove_tag_n(const char *key, size_t key_len);

/**
 * Sets extra information.
 */
SENTRY_API void sentry_set_extra(const char *key, sentry_value_t value);
SENTRY_API void sentry_set_extra_n(
    const char *key, size_t key_len, sentry_value_t value);
SENTRY_API void sentry_scope_set_extra(
    sentry_scope_t *scope, const char *key, sentry_value_t value);
SENTRY_API void sentry_scope_set_extra_n(sentry_scope_t *scope, const char *key,
    size_t key_len, sentry_value_t value);

/**
 * Removes the extra with the specified key.
 */
SENTRY_API void sentry_remove_extra(const char *key);
SENTRY_API void sentry_remove_extra_n(const char *key, size_t key_len);

/**
 * Sets attributes created with `sentry_value_new_attribute` to be applied to
 * all:
 * - logs
 * - metrics
 */
SENTRY_API void sentry_set_attribute(const char *key, sentry_value_t attribute);
SENTRY_API void sentry_set_attribute_n(
    const char *key, size_t key_len, sentry_value_t attribute);
SENTRY_API void sentry_remove_attribute(const char *key);
SENTRY_API void sentry_remove_attribute_n(const char *key, size_t key_len);

/**
 * Sets a context object.
 */
SENTRY_API void sentry_set_context(const char *key, sentry_value_t value);
SENTRY_API void sentry_set_context_n(
    const char *key, size_t key_len, sentry_value_t value);
SENTRY_API void sentry_scope_set_context(
    sentry_scope_t *scope, const char *key, sentry_value_t value);
SENTRY_API void sentry_scope_set_context_n(sentry_scope_t *scope,
    const char *key, size_t key_len, sentry_value_t value);
/**
 * Updates a context object by merging the provided values into it.
 * Keys in `value` take precedence over keys in the context, updating the
 * existing values. Keys in the existing context that are not present in `value`
 * are preserved. If the context object does not exist yet, it will be created.
 */
SENTRY_API void sentry_update_context(const char *key, sentry_value_t value);
SENTRY_API void sentry_update_context_n(
    const char *key, size_t key_len, sentry_value_t value);
SENTRY_API void sentry_scope_update_context(
    sentry_scope_t *scope, const char *key, sentry_value_t value);
SENTRY_API void sentry_scope_update_context_n(sentry_scope_t *scope,
    const char *key, size_t key_len, sentry_value_t value);

/**
 * Removes the context object with the specified key.
 */
SENTRY_API void sentry_remove_context(const char *key);
SENTRY_API void sentry_remove_context_n(const char *key, size_t key_len);

/**
 * Sets the event fingerprint.
 *
 * This accepts a variable number of arguments and needs to be terminated by a
 * trailing `NULL`. Each string argument in the `_n` variants must be followed
 * by its length.
 */
SENTRY_API void sentry_set_fingerprint(const char *fingerprint, ...);
SENTRY_API void sentry_set_fingerprint_n(
    const char *fingerprint, size_t fingerprint_len, ...);
SENTRY_API void sentry_scope_set_fingerprint(
    sentry_scope_t *scope, const char *fingerprint, ...);
SENTRY_API void sentry_scope_set_fingerprint_n(sentry_scope_t *scope,
    const char *fingerprint, size_t fingerprint_len, ...);

/**
 * Sets the event fingerprints.
 *
 * This accepts a list of fingerprints created with `sentry_value_new_list`.
 */
SENTRY_API void sentry_scope_set_fingerprints(
    sentry_scope_t *scope, sentry_value_t fingerprints);

/**
 * Removes the fingerprint.
 */
SENTRY_API void sentry_remove_fingerprint(void);

/**
 * Set the trace. The primary use for this is to allow other SDKs to propagate
 * their trace context to connect events on all layers.
 *
 * Once a trace is managed by the downstream SDK using this function,
 * transactions no longer act as automatic trace boundaries.
 */
SENTRY_API void sentry_set_trace(
    const char *trace_id, const char *parent_span_id);
SENTRY_API void sentry_set_trace_n(const char *trace_id, size_t trace_id_len,
    const char *parent_span_id, size_t parent_span_id_len);

/**
 * Generates a new random `trace_id` and `span_id` and sets these onto
 * the propagation context. Use this to set a trace boundary for
 * events/transactions.
 *
 * Once you regenerate a trace manually, transactions no longer act as automatic
 * trace boundaries. This means all following transactions will be part of the
 * same trace until you regenerate the trace again.
 *
 * We urge you not to use this function if you use the Native SDK in the context
 * of a downstream SDK like Android, .NET, Unity, or Unreal, because it will
 * interfere with cross-SDK traces which are managed by these SDKs.
 */
SENTRY_EXPERIMENTAL_API void sentry_regenerate_trace(void);

/**
 * Sets the transaction.
 */
SENTRY_API void sentry_set_transaction(const char *transaction);
SENTRY_API void sentry_set_transaction_n(
    const char *transaction, size_t transaction_len);

/**
 * Sets the event level.
 */
SENTRY_API void sentry_set_level(sentry_level_t level);
SENTRY_API void sentry_scope_set_level(
    sentry_scope_t *scope, sentry_level_t level);

/**
 * Sets the maximum number of spans that can be attached to a
 * transaction.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_max_spans(
    sentry_options_t *opts, size_t max_spans);

/**
 * Gets the maximum number of spans that can be attached to a
 * transaction.
 */
SENTRY_EXPERIMENTAL_API size_t sentry_options_get_max_spans(
    sentry_options_t *opts);

/**
 * Sets the sample rate for transactions. Should be a double between
 * `0.0` and `1.0`. Transactions will be randomly discarded during
 * `sentry_transaction_finish` when the sample rate is < 1.0.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_traces_sample_rate(
    sentry_options_t *opts, double sample_rate);

/**
 * Returns the sample rate for transactions.
 */
SENTRY_EXPERIMENTAL_API double sentry_options_get_traces_sample_rate(
    sentry_options_t *opts);

/**
 * A sentry Transaction Context.
 *
 * See Transaction Interface under
 * https://develop.sentry.dev/sdk/performance/#new-span-and-transaction-classes
 */
struct sentry_transaction_context_s;
typedef struct sentry_transaction_context_s sentry_transaction_context_t;
typedef double (*sentry_traces_sampler_function)(
    const sentry_transaction_context_t *transaction_ctx,
    sentry_value_t custom_sampling_ctx, const int *parent_sampled,
    void *user_data);

/**
 * Sets the traces sampler callback. Should be a function that returns a double
 * and takes in a sentry_transaction_context_t pointer, a sentry_value_t for
 * a custom sampling context int pointer for the parent sampled flag, and some
 * optional user_data.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_traces_sampler(
    sentry_options_t *opts, sentry_traces_sampler_function callback,
    void *user_data);

/**
 * Enables or disables propagation of the W3C Trace Context `traceparent`
 * header.
 *
 * When enabled, the `traceparent` header will be included alongside the
 * `sentry-trace` header in outgoing HTTP requests for distributed tracing
 * interoperability with OpenTelemetry (OTel) services.
 *
 * This is disabled by default.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_propagate_traceparent(
    sentry_options_t *opts, int propagate_traceparent);

/**
 * Returns whether W3C Trace Context `traceparent` header propagation is
 * enabled.
 */
SENTRY_EXPERIMENTAL_API int sentry_options_get_propagate_traceparent(
    const sentry_options_t *opts);

/**
 * Overrides the organization ID derived from the DSN host
 * (e.g. `o123456.ingest.sentry.io` → `123456`). Typically only required for
 * self-hosted setups where the DSN host does not encode the organization ID.
 *
 * The value is passed through as a string; no validation is performed.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_org_id(
    sentry_options_t *opts, const char *org_id);
SENTRY_EXPERIMENTAL_API void sentry_options_set_org_id_n(
    sentry_options_t *opts, const char *org_id, size_t org_id_len);

/**
 * Enables or disables strict trace continuation.
 *
 * Controls whether to continue an incoming trace when either the trace or the
 * SDK has an organization ID (derived from the DSN), but not both. When set
 * to true, a new trace is started in that case; when false, the incoming
 * trace is continued. If both organization IDs are present and differ, the
 * trace is never continued regardless of this setting.
 *
 * See
 * https://develop.sentry.dev/sdk/foundations/trace-propagation/#strict-trace-continuation
 *
 * This is disabled by default.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_strict_trace_continuation(
    sentry_options_t *opts, int strict_trace_continuation);

/**
 * Returns whether strict trace continuation is enabled.
 */
SENTRY_EXPERIMENTAL_API int sentry_options_get_strict_trace_continuation(
    const sentry_options_t *opts);

/**
 * Enables or disables the structured logging feature.
 * When disabled, all calls to `sentry_log_X()` are no-ops.
 *
 * Enabled by default.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_enable_logs(
    sentry_options_t *opts, int enable_logs);
SENTRY_EXPERIMENTAL_API int sentry_options_get_enable_logs(
    const sentry_options_t *opts);

/**
 * Enables or disables HTTP retry with exponential backoff for network failures.
 *
 * Only applicable for HTTP transports.
 *
 * Disabled by default.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_http_retry(
    sentry_options_t *opts, int enabled);
SENTRY_EXPERIMENTAL_API int sentry_options_get_http_retry(
    const sentry_options_t *opts);

/**
 * Enables or disables out-of-band upload of large attachments.
 *
 * When enabled, attachments above an internal size threshold are uploaded
 * via a separate request before the envelope is sent, and referenced from
 * the envelope by location instead of being embedded inline. When disabled,
 * attachments are embedded in the envelope and oversized uploads are rejected
 * by Sentry.
 *
 * Note: Requires the `projects:relay-upload-endpoint` server feature. See
 * https://develop.sentry.dev/sdk/telemetry/attachments/#attachment-placeholders
 *
 * Disabled by default.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_enable_large_attachments(
    sentry_options_t *opts, int enable_large_attachments);
SENTRY_EXPERIMENTAL_API int sentry_options_get_enable_large_attachments(
    const sentry_options_t *opts);

/**
 * Enables or disables custom attributes parsing for structured logging.
 *
 * When enabled, all `sentry_log_X()` functions expect a `sentry_value_t` object
 * as the first variadic argument for custom log attributes. Remaining
 * arguments are used for format string substitution.
 *
 * Disabled by default.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_logs_with_attributes(
    sentry_options_t *opts, int logs_with_attributes);
SENTRY_EXPERIMENTAL_API int sentry_options_get_logs_with_attributes(
    const sentry_options_t *opts);

/**
 * Enables or disables client reports.
 *
 * Client reports allow the SDK to track and report why events were discarded
 * before being sent to Sentry (e.g., due to sampling, hooks, rate limiting,
 * network or send errors, queue overflow, etc.).
 *
 * When enabled (the default), client reports are opportunistically attached to
 * outgoing envelopes to minimize HTTP requests.
 *
 * See https://develop.sentry.dev/sdk/telemetry/client-reports/ for details.
 */
SENTRY_API void sentry_options_set_send_client_reports(
    sentry_options_t *opts, int val);

/**
 * Returns true if client reports are enabled.
 */
SENTRY_API int sentry_options_get_send_client_reports(
    const sentry_options_t *opts);

/**
 * The potential returns of calling any of the sentry_log_X functions
 * - Success means a log was enqueued
 * - Discard means the `before_send_log` function discarded the log
 * - Failed means the log wasn't enqueued. This happens if the buffers are full
 * - Disabled means the option `enable_logs` was false.
 */
typedef enum {
    SENTRY_LOG_RETURN_SUCCESS = 0,
    SENTRY_LOG_RETURN_DISCARD = 1,
    SENTRY_LOG_RETURN_FAILED = 2,
    SENTRY_LOG_RETURN_DISABLED = 3
} log_return_value_t;

/**
 * Structured logging interface. Minimally blocks the client trying to log,
 * but is therefore lossy when enqueueing a log fails
 * (e.g., when both buffers are full).
 *
 * Format string restrictions:
 * Only a subset of printf format specifiers are supported for parameter
 * extraction. Supported specifiers include:
 * - %d, %i - signed integers (treated as long long)
 * - %u, %x, %X, %o - unsigned integers (treated as unsigned long long)
 * - %f, %F, %e, %E, %g, %G - floating point numbers (treated as double)
 * - %c - single character
 * - %s - null-terminated string (null pointers are handled as "(null)")
 * - %p - pointer value (formatted as hexadecimal string)
 *
 * Unsupported format specifiers will consume their corresponding argument
 * but will be recorded as "(unknown)" in the structured log data.
 * Length modifiers (h, l, L, z, j, t) are parsed but ignored.
 *
 * Because of this, please only use 64-bit types/casts for your arguments.
 *
 * Flags, width, and precision specifiers are parsed but currently ignored for
 * parameter extraction purposes.
 *
 * When the option `logs_with_attributes` is enabled, the first varg is parsed
 * as a `sentry_value_t` object containing the initial attributes for the log.
 * You can pass `sentry_value_new_null()` to logs which don't need attributes.
 *
 * Ownership of the attributes is transferred to the log function.
 *
 * To re-use the same attributes, call `sentry_value_incref` on it
 * before passing the attributes to the log function.
 */
SENTRY_EXPERIMENTAL_API log_return_value_t sentry_log_trace(
    const char *message, ...);
SENTRY_EXPERIMENTAL_API log_return_value_t sentry_log_debug(
    const char *message, ...);
SENTRY_EXPERIMENTAL_API log_return_value_t sentry_log_info(
    const char *message, ...);
SENTRY_EXPERIMENTAL_API log_return_value_t sentry_log_warn(
    const char *message, ...);
SENTRY_EXPERIMENTAL_API log_return_value_t sentry_log_error(
    const char *message, ...);
SENTRY_EXPERIMENTAL_API log_return_value_t sentry_log_fatal(
    const char *message, ...);

/**
 * Sends a structured log with a plain string body and explicit attributes.
 *
 * Unlike the `sentry_log_*` functions, this function does NOT interpret the
 * body as a printf format string. The body is stored as-is, making it safe
 * for user-provided text that may contain `%` characters.
 *
 * Ownership of the `attributes` value is transferred to this function.
 * Pass `sentry_value_new_null()` if no custom attributes are needed.
 * To re-use the same attributes, call `sentry_value_incref` before passing.
 */
SENTRY_EXPERIMENTAL_API log_return_value_t sentry_log(
    sentry_level_t level, const char *body, sentry_value_t attributes);

/**
 * Type of the `before_send_log` callback.
 *
 * The callback takes ownership of the `log` and should usually return
 * that same log. In case the log should be discarded, the
 * callback needs to call `sentry_value_decref` on the provided log and
 * return a `sentry_value_new_null()` instead.
 */
typedef sentry_value_t (*sentry_before_send_log_function_t)(
    sentry_value_t log, void *user_data);

/**
 * Sets the `before_send_log` callback.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_before_send_log(
    sentry_options_t *opts, sentry_before_send_log_function_t func, void *data);

/**
 * Enables or disables the metrics feature.
 * When disabled, all calls to `sentry_metrics_*()` are no-ops.
 *
 * Enabled by default.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_enable_metrics(
    sentry_options_t *opts, int enable_metrics);
SENTRY_EXPERIMENTAL_API int sentry_options_get_enable_metrics(
    const sentry_options_t *opts);

/**
 * Enables or disables in-process app-hang detection. When enabled, a
 * background watchdog thread monitors heartbeats from the watched thread. If
 * no heartbeat is received within the configured timeout, an app-hang event is
 * captured and sent to Sentry.
 *
 * Disabled by default. Must be combined with regular calls to
 * `sentry_app_hang_heartbeat()` from the thread you want monitored.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_enable_app_hang_tracking(
    sentry_options_t *opts, int enable);
SENTRY_EXPERIMENTAL_API int sentry_options_get_enable_app_hang_tracking(
    const sentry_options_t *opts);

/**
 * Sets the app-hang detection timeout (in milliseconds). Defaults to 5000 ms.
 * If `enable_app_hang_tracking` is true and no heartbeat is received within
 * this window, an app-hang event is captured. Detection is enabled/disabled via
 * `enable_app_hang_tracking`, not via this timeout.
 *
 * The watchdog samples the heartbeat on a fixed internal interval, so timeouts
 * shorter than 1000 ms cannot be resolved reliably. Values below that minimum
 * are clamped up to it (with a warning).
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_app_hang_timeout(
    sentry_options_t *opts, uint64_t app_hang_timeout);
SENTRY_EXPERIMENTAL_API uint64_t sentry_options_get_app_hang_timeout(
    const sentry_options_t *opts);

/**
 * Records a heartbeat from the calling thread.
 *
 * The first call latches the calling thread as the monitored thread.
 * Call this regularly from the thread you want watched for hangs. If the
 * watchdog does not receive a heartbeat within the configured timeout, it
 * captures an app-hang event.
 *
 * Only a single thread is monitored: the watchdog tracks exactly the one
 * thread latched by the first heartbeat. Calls from any other thread are
 * ignored. To watch the thread most representative of responsiveness (e.g.
 * a UI or main loop), call this from that thread first.
 *
 * This function is a no-op unless app-hang detection is enabled via
 * `sentry_options_set_enable_app_hang_tracking`.
 */
SENTRY_EXPERIMENTAL_API void sentry_app_hang_heartbeat(void);

/**
 * Type of the `before_send_metric` callback.
 *
 * The callback takes ownership of the `metric` and should usually return
 * that same metric. In case the metric should be discarded, the
 * callback needs to call `sentry_value_decref` on the provided metric and
 * return a `sentry_value_new_null()` instead.
 */
typedef sentry_value_t (*sentry_before_send_metric_function_t)(
    sentry_value_t metric, void *user_data);

/**
 * Sets the `before_send_metric` callback.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_before_send_metric(
    sentry_options_t *opts, sentry_before_send_metric_function_t func,
    void *data);

/**
 * Result type for metric operations.
 * - Success means the metric was enqueued
 * - Discard means the `before_send_metric` callback discarded the metric
 * - Failed means the metric wasn't enqueued (buffers are full)
 * - Disabled means metrics are disabled
 */
typedef enum {
    SENTRY_METRICS_RESULT_SUCCESS = 0,
    SENTRY_METRICS_RESULT_DISCARD = 1,
    SENTRY_METRICS_RESULT_FAILED = 2,
    SENTRY_METRICS_RESULT_DISABLED = 3
} sentry_metrics_result_t;

/**
 * Metrics interface for recording application metrics.
 *
 * Metrics are buffered and sent in batches. Each metric includes:
 * - name: Hierarchical name with dot separators (e.g., "api.requests")
 * - value: The numeric value to record
 * - unit: Optional measurement unit (e.g., SENTRY_UNIT_MILLISECOND), or NULL
 * - attributes: Optional sentry_value_t object with custom attributes, or
 *   sentry_value_new_null(). Each attribute should be created with
 *   sentry_value_new_attribute().
 *
 * Ownership of the attributes is transferred to the metric function.
 *
 * To re-use the same attributes, call `sentry_value_incref` on it
 * before passing the attributes to the metric function.
 *
 * Metrics are automatically associated with the current trace and span if
 * available. Default attributes (environment, release, SDK info) are attached
 * automatically.
 */

/**
 * Records a counter metric. Counters track incrementing values like
 * request counts or error counts.
 */
SENTRY_EXPERIMENTAL_API sentry_metrics_result_t sentry_metrics_count(
    const char *name, int64_t value, sentry_value_t attributes);

/**
 * Records a gauge metric. Gauges track values that can go up or down,
 * like memory usage or active connections.
 */
SENTRY_EXPERIMENTAL_API sentry_metrics_result_t sentry_metrics_gauge(
    const char *name, double value, const char *unit,
    sentry_value_t attributes);

/**
 * Records a distribution metric. Distributions track the statistical
 * distribution of values, useful for timing data and percentiles.
 */
SENTRY_EXPERIMENTAL_API sentry_metrics_result_t sentry_metrics_distribution(
    const char *name, double value, const char *unit,
    sentry_value_t attributes);

#ifdef SENTRY_PLATFORM_LINUX

/**
 * Returns the currently set strategy for the handler.
 *
 * This option does only work for the `inproc` backend on `Linux` and `Android`.
 *
 * The main use-case is when the Native SDK is used in the context of the
 * CLR/Mono runtimes which convert some POSIX signals into managed-code
 * exceptions and discontinue the signal chain.
 *
 * If this happens, and we invoke the previous handler at the end (i.e., after
 * our handler processed the signal, which is the default strategy), we will end
 * up sending a native crash in addition to the managed code exception. This
 * will either generate another crash event if uncaught or could be handled in
 * the managed code and neither terminate the application nor create a crash
 * event. To correctly process the signals of CLR/Mono applications, we must
 * invoke the runtime handler at the start of our handler.
 */
SENTRY_EXPERIMENTAL_API sentry_handler_strategy_t
sentry_options_get_handler_strategy(const sentry_options_t *opts);

/**
 * Sets the handler strategy.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_handler_strategy(
    sentry_options_t *opts, sentry_handler_strategy_t handler_strategy);

#endif // SENTRY_PLATFORM_LINUX

/**
 * A sentry Attachment.
 *
 * See https://develop.sentry.dev/sdk/data-model/envelope-items/#attachment
 */
struct sentry_attachment_s;
typedef struct sentry_attachment_s sentry_attachment_t;

/**
 * Attaches a file to be sent along with events.
 *
 * `path` is assumed to be in a platform-specific filesystem path encoding.
 * API Users on windows are encouraged to use `sentry_attach_filew` or
 * `sentry_scope_attach_filew` instead.
 *
 * The same file cannot be attached multiple times i.e. `path` must be unique.
 * Calling this function multiple times with the same `path` is safe, but
 * duplicate attachments with equal paths will not be added.
 *
 * The returned `sentry_attachment_t` is owned by the SDK and will remain valid
 * until the attachment is removed with `sentry_remove_attachment` or
 * `sentry_close` is called.
 *
 * See the NOTE on attachments above for restrictions of this API.
 */
SENTRY_API sentry_attachment_t *sentry_attach_file(const char *path);
SENTRY_API sentry_attachment_t *sentry_attach_file_n(
    const char *path, size_t path_len);
SENTRY_API sentry_attachment_t *sentry_scope_attach_file(
    sentry_scope_t *scope, const char *path);
SENTRY_API sentry_attachment_t *sentry_scope_attach_file_n(
    sentry_scope_t *scope, const char *path, size_t path_len);

/**
 * Attaches bytes to be sent along with events.
 *
 * `filename` is assumed to be in a platform-specific filesystem path encoding.
 * API Users on windows are encouraged to use `sentry_attach_bytesw` or
 * `sentry_scope_attach_bytesw` instead.
 *
 * `filename` is used to identify the attachment in the Sentry Web UI. It is
 * recommended to use unique filenames to make attachments easier to
 * differentiate. However, neither `filename` nor `buf` is used to reject
 * duplicate attachments.
 *
 * NOTE: When using the `crashpad` backend, it writes byte attachments to disk
 * into a flat directory structure. If multiple buffers are attached with the
 * same `filename`, it will internally ensure unique filenames for attachments
 * by appending a unique suffix to the filename. Therefore, attachments may show
 * up with altered names in the Sentry Web UI.
 *
 * The returned `sentry_attachment_t` is owned by the SDK and will remain valid
 * until the attachment is removed with `sentry_remove_attachment` or
 * `sentry_close` is called.
 *
 * See the NOTE on attachments above for restrictions of this API.
 */
SENTRY_API sentry_attachment_t *sentry_attach_bytes(
    const char *buf, size_t buf_len, const char *filename);
SENTRY_API sentry_attachment_t *sentry_attach_bytes_n(
    const char *buf, size_t buf_len, const char *filename, size_t filename_len);
SENTRY_API sentry_attachment_t *sentry_scope_attach_bytes(sentry_scope_t *scope,
    const char *buf, size_t buf_len, const char *filename);
SENTRY_API sentry_attachment_t *sentry_scope_attach_bytes_n(
    sentry_scope_t *scope, const char *buf, size_t buf_len,
    const char *filename, size_t filename_len);

/**
 * Removes and frees all previously added attachments.
 */
SENTRY_API void sentry_clear_attachments(void);

/**
 * Removes and frees a previously added attachment.
 *
 * See the NOTE on attachments above for restrictions of this API.
 */
SENTRY_API void sentry_remove_attachment(sentry_attachment_t *attachment);

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Wide char versions of `sentry_attach_file` and `sentry_scope_attach_file`.
 */
SENTRY_API sentry_attachment_t *sentry_attach_filew(const wchar_t *path);
SENTRY_API sentry_attachment_t *sentry_attach_filew_n(
    const wchar_t *path, size_t path_len);
SENTRY_API sentry_attachment_t *sentry_scope_attach_filew(
    sentry_scope_t *scope, const wchar_t *path);
SENTRY_API sentry_attachment_t *sentry_scope_attach_filew_n(
    sentry_scope_t *scope, const wchar_t *path, size_t path_len);

/**
 * Wide char versions of `sentry_attach_bytes` and `sentry_scope_attach_bytes`.
 */
SENTRY_API sentry_attachment_t *sentry_attach_bytesw(
    const char *buf, size_t buf_len, const wchar_t *filename);
SENTRY_API sentry_attachment_t *sentry_attach_bytesw_n(const char *buf,
    size_t buf_len, const wchar_t *filename, size_t filename_len);
SENTRY_API sentry_attachment_t *sentry_scope_attach_bytesw(
    sentry_scope_t *scope, const char *buf, size_t buf_len,
    const wchar_t *filename);
SENTRY_API sentry_attachment_t *sentry_scope_attach_bytesw_n(
    sentry_scope_t *scope, const char *buf, size_t buf_len,
    const wchar_t *filename, size_t filename_len);
#endif

#define SENTRY_ATTACHMENT_TYPE_GENERIC "event.attachment"
#define SENTRY_ATTACHMENT_TYPE_MINIDUMP "event.minidump"
#define SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY "event.view_hierarchy"

/**
 * Sets the attachment type.
 *
 * Well-known attachment types are exposed as `SENTRY_ATTACHMENT_TYPE_*`
 * macros. Pass `NULL` or an empty string to clear the attachment type.
 *
 * Also sets the content type for known attachment types unless explicitly set.
 *
 * See:
 * https://develop.sentry.dev/sdk/telemetry/attachments/#attachment-types
 */
SENTRY_API void sentry_attachment_set_type(
    sentry_attachment_t *attachment, const char *type);
SENTRY_API void sentry_attachment_set_type_n(
    sentry_attachment_t *attachment, const char *type, size_t type_len);

/**
 * Sets the content type of attachment.
 */
SENTRY_API void sentry_attachment_set_content_type(
    sentry_attachment_t *attachment, const char *content_type);
SENTRY_API void sentry_attachment_set_content_type_n(
    sentry_attachment_t *attachment, const char *content_type,
    size_t content_type_len);

/**
 * Sets the filename of an attachment.
 */
SENTRY_API void sentry_attachment_set_filename(
    sentry_attachment_t *attachment, const char *filename);
SENTRY_API void sentry_attachment_set_filename_n(
    sentry_attachment_t *attachment, const char *filename, size_t filename_len);

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Wide char version of `sentry_attachment_set_filename`.
 */
SENTRY_API void sentry_attachment_set_filenamew(
    sentry_attachment_t *attachment, const wchar_t *filename);
SENTRY_API void sentry_attachment_set_filenamew_n(
    sentry_attachment_t *attachment, const wchar_t *filename,
    size_t filename_len);
#endif

/* -- Session APIs -- */

typedef enum {
    SENTRY_SESSION_STATUS_OK,
    SENTRY_SESSION_STATUS_CRASHED,
    SENTRY_SESSION_STATUS_ABNORMAL,
    SENTRY_SESSION_STATUS_EXITED,
} sentry_session_status_t;

/**
 * Starts a new session.
 */
SENTRY_API void sentry_start_session(void);

/**
 * Ends a session.
 */
SENTRY_API void sentry_end_session(void);

/**
 * Ends a session with an explicit `status` code.
 */
SENTRY_EXPERIMENTAL_API void sentry_end_session_with_status(
    sentry_session_status_t status);

/* -- Performance Monitoring/Tracing APIs -- */

/**
 * A sentry Transaction.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/transaction/
 */
struct sentry_transaction_s;
typedef struct sentry_transaction_s sentry_transaction_t;

/**
 * Type of the `before_transaction` callback.
 *
 * The callback takes ownership of the `transaction`, and should usually return
 * that same transaction. In case the transaction should be discarded, the
 * callback needs to call `sentry_value_decref` on the provided transaction and
 * return a `sentry_value_new_null()` instead.
 *
 * This function may be invoked inside a signal handler and must be safe for
 * that purpose, see https://man7.org/linux/man-pages/man7/signal-safety.7.html.
 * On Windows, it may be called from inside a `UnhandledExceptionFilter`, see
 * the documentation on SEH (structured exception handling) for more information
 * https://docs.microsoft.com/en-us/windows/win32/debug/structured-exception-handling
 */
typedef sentry_value_t (*sentry_transaction_function_t)(
    sentry_value_t transaction, void *user_data);

/**
 * Sets the `before_transaction` callback.
 */
SENTRY_EXPERIMENTAL_API void sentry_options_set_before_transaction(
    sentry_options_t *opts, sentry_transaction_function_t func, void *data);

/**
 * A sentry Span.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/span/
 */
struct sentry_span_s;
typedef struct sentry_span_s sentry_span_t;

/**
 * Constructs a new Transaction Context. The returned value needs to be passed
 * into `sentry_transaction_start` in order to be recorded and sent to sentry.
 *
 * See
 * https://docs.sentry.io/platforms/native/enriching-events/transaction-name/
 * for an explanation of a Transaction's `name`, and
 * https://develop.sentry.dev/sdk/performance/span-operations/ for conventions
 * around an `operation`'s value.
 *
 * Also see https://develop.sentry.dev/sdk/event-payloads/transaction/#anatomy
 * for an explanation of `operation`, in addition to other properties and
 * actions that can be performed on a Transaction.
 *
 * The returned value is not thread-safe. Users are expected to ensure that
 * appropriate locking mechanisms are implemented over the Transaction Context
 * if it needs to be mutated across threads. Methods operating on the
 * Transaction Context will mention what kind of expectations they carry if they
 * need to mutate or access the object in a thread-safe way.
 */
SENTRY_EXPERIMENTAL_API sentry_transaction_context_t *
sentry_transaction_context_new(const char *name, const char *operation);
SENTRY_EXPERIMENTAL_API sentry_transaction_context_t *
sentry_transaction_context_new_n(const char *name, size_t name_len,
    const char *operation, size_t operation_len);

/**
 * Sets the `name` on a Transaction Context, which will be used in the
 * Transaction constructed off of the context.
 *
 * The Transaction Context should not be mutated by other functions while
 * setting a name on it.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_set_name(
    sentry_transaction_context_t *tx_ctx, const char *name);
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_set_name_n(
    sentry_transaction_context_t *tx_ctx, const char *name, size_t name_len);
/**
 * Gets the `name` of a Transaction Context.
 */
SENTRY_EXPERIMENTAL_API const char *sentry_transaction_context_get_name(
    const sentry_transaction_context_t *tx_ctx);

/**
 * Sets the `operation` on a Transaction Context, which will be used in the
 * Transaction constructed off of the context
 *
 * See https://develop.sentry.dev/sdk/performance/span-operations/ for
 * conventions on `operation`s.
 *
 * The Transaction Context should not be mutated by other functions while
 * setting an operation on it.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_set_operation(
    sentry_transaction_context_t *tx_ctx, const char *operation);
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_set_operation_n(
    sentry_transaction_context_t *tx_ctx, const char *operation,
    size_t operation_len);
/**
 * Gets the `operation` of a Transaction Context.
 */
SENTRY_EXPERIMENTAL_API const char *sentry_transaction_context_get_operation(
    const sentry_transaction_context_t *tx_ctx);

/**
 * Sets the `sampled` field on a Transaction Context, which will be used in the
 * Transaction constructed off of the context.
 *
 * When passing any value above 0, the Transaction will bypass all sampling
 * options and always be sent to sentry. If passed 0, this Transaction and its
 * child spans will never be sent to sentry.
 *
 * The Transaction Context should not be mutated by other functions while
 * setting `sampled` on it.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_set_sampled(
    sentry_transaction_context_t *tx_ctx, int sampled);

/**
 * Removes the `sampled` field on a Transaction Context, which will be used in
 * the Transaction constructed off of the context.
 *
 * The Transaction will use the sampling rate as defined in `sentry_options`.
 *
 * The Transaction Context should not be mutated by other functions while
 * removing `sampled`.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_remove_sampled(
    sentry_transaction_context_t *tx_ctx);

/**
 * Update the Transaction Context with the given HTTP header key/value pair.
 *
 * This is used to propagate distributed tracing metadata from upstream
 * services. Therefore, the headers of incoming requests should be fed into this
 * function so that sentry is able to continue a trace that was started by an
 * upstream service.
 *
 * Recognized header keys are `sentry-trace` and `baggage` (case-insensitive);
 * other keys are ignored. Feed both when available so that strict trace
 * continuation can consult the incoming `sentry-org_id`.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_update_from_header(
    sentry_transaction_context_t *tx_ctx, const char *key, const char *value);
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_update_from_header_n(
    sentry_transaction_context_t *tx_ctx, const char *key, size_t key_len,
    const char *value, size_t value_len);

/**
 * Starts a new Transaction based on the provided context, restored from an
 * external integration (i.e. a span from a different SDK) or manually
 * constructed by a user.
 *
 * The second parameter is a custom Sampling Context to be used with a Traces
 * Sampler to allow you to make a more informed sampling decision.
 *
 * Returns a Transaction, which is expected to be manually managed by the
 * caller. Manual management involves ensuring that `sentry_transaction_finish`
 * is invoked for the Transaction, and that the caller manually starts and
 * finishes any child Spans as needed on the Transaction.
 *
 * Not invoking `sentry_transaction_finish` with the returned Transaction means
 * it will be discarded and will not be sent to sentry.
 *
 * To ensure that any Events or Message Events are associated with this
 * Transaction while it is active, invoke and pass in the Transaction returned
 * by this function to `sentry_set_transaction_object`. Further documentation on
 * this can be found in `sentry_set_transaction_object`'s docstring.
 *
 * Takes ownership of `transaction_context`. A Transaction Context cannot be
 * modified or re-used after it is used to start a Transaction.
 *
 * Takes ownership of `custom_sampling_ctx`. A Sampling Context cannot be
 * modified or re-used after it is used to start a Transaction.
 *
 * The returned value is not thread-safe. Users are expected to ensure that
 * appropriate locking mechanisms are implemented over the Transaction if it
 * needs to be mutated across threads. Methods operating on the Transaction will
 * mention what kind of expectations they carry if they need to mutate or access
 * the object in a thread-safe way.
 */
SENTRY_EXPERIMENTAL_API sentry_transaction_t *sentry_transaction_start(
    sentry_transaction_context_t *tx_ctx, sentry_value_t custom_sampling_ctx);
/**
 * Also starts a transaction like the regular `sentry_transaction_start`
 * function but has an additional timestamp parameter to let the user provide
 * explicit timings.
 *
 * The timestamp must be in microseconds passed since the Unix epoch.
 */
SENTRY_EXPERIMENTAL_API sentry_transaction_t *sentry_transaction_start_ts(
    sentry_transaction_context_t *tx_ctx, sentry_value_t custom_sampling_ctx,
    uint64_t timestamp);

/**
 * Finishes and sends a Transaction to sentry. The event ID of the Transaction
 * will be returned if this was successful; A nil UUID will be returned
 * otherwise.
 *
 * Always takes ownership of `transaction`, regardless of whether the operation
 * was successful or not. A Transaction cannot be modified or re-used after it
 * is finished.
 */
SENTRY_EXPERIMENTAL_API sentry_uuid_t sentry_transaction_finish(
    sentry_transaction_t *tx);
/**
 * Also finishes a transaction like the regular `sentry_transaction_finish`
 * function but has an additional timestamp parameter to let the user provide
 * explicit timings.
 *
 * The timestamp must be in microseconds passed since the Unix epoch.
 */
SENTRY_EXPERIMENTAL_API sentry_uuid_t sentry_transaction_finish_ts(
    sentry_transaction_t *tx, uint64_t timestamp);

/**
 * Sets the Transaction so any Events sent while the Transaction
 * is active will be associated with the Transaction.
 *
 * If the Transaction being passed in is unsampled, it will still be associated
 * with any new Events. This will lead to some Events pointing to orphan or
 * missing traces in sentry, see
 * https://docs.sentry.io/product/sentry-basics/tracing/trace-view/#orphan-traces-and-broken-subtraces
 *
 * This increases the number of references pointing to the Transaction. Invoke
 * `sentry_transaction_finish` to remove the Transaction set by this function as
 * well as its reference by passing in the same Transaction as the one passed
 * into this function.
 */
SENTRY_EXPERIMENTAL_API void sentry_set_transaction_object(
    sentry_transaction_t *tx);

/**
 * Sets the Span so any Events sent while the Span
 * is active will be associated with the Span.
 *
 * This increases the number of references pointing to the Span. Invoke
 * `sentry_span_finish` to remove the Span set by this function as well
 * as its reference by passing in the same Span as the one passed into
 * this function.
 */
SENTRY_EXPERIMENTAL_API void sentry_set_span(sentry_span_t *span);

/**
 * Starts a new Span.
 *
 * The return value of `sentry_transaction_start` should be passed in as
 * `parent`. This value can't be null, since we don't allow for orphan spans.
 *
 * Both `operation` and `description` can be null, but it is recommended to
 * supply the former. See
 * https://develop.sentry.dev/sdk/performance/span-operations/ for conventions
 * around operations.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/span/ for a description of
 * the created Span's properties and expectations for `operation` and
 * `description`.
 *
 * Returns a value that should be passed into `sentry_span_finish`. Not
 * finishing the Span means it will be discarded and will not be sent to
 * sentry. `sentry_value_null` will be returned if the child Span could not be
 * created.
 *
 * To ensure that any Events or Message Events are associated with this
 * Span while it is active, invoke and pass in the Span returned
 * by this function to `sentry_set_span`. Further documentation on this can be
 * found in `sentry_set_span`'s docstring.
 *
 * This increases the number of references pointing to the Transaction.
 *
 * The returned value is not thread-safe. Users are expected to ensure that
 * appropriate locking mechanisms are implemented over the Span if it needs
 * to be mutated across threads. Methods operating on the Span will mention what
 * kind of expectations they carry if they need to mutate or access the object
 * in a thread-safe way.
 */
SENTRY_EXPERIMENTAL_API sentry_span_t *sentry_transaction_start_child(
    sentry_transaction_t *parent, const char *operation,
    const char *description);
SENTRY_EXPERIMENTAL_API sentry_span_t *sentry_transaction_start_child_n(
    sentry_transaction_t *parent, const char *operation, size_t operation_len,
    const char *description, size_t description_len);
/**
 * Also starts a span like the regular `sentry_transaction_start_child_ts`
 * functions but has an additional timestamp parameter to let the user provide
 * explicit timings.
 *
 * The timestamp must be in microseconds passed since the Unix epoch.
 */
SENTRY_EXPERIMENTAL_API sentry_span_t *sentry_transaction_start_child_ts(
    sentry_transaction_t *parent, const char *operation,
    const char *description, uint64_t timestamp);
SENTRY_EXPERIMENTAL_API sentry_span_t *sentry_transaction_start_child_ts_n(
    sentry_transaction_t *parent, const char *operation, size_t operation_len,
    const char *description, size_t description_len, uint64_t timestamp);

/**
 * Starts a new Span.
 *
 * The return value of either `sentry_transaction_start_child` or
 * `sentry_span_start_child` should be passed in as `parent`. This value can't
 * be null, since we don't allow for orphan spans.
 *
 * Both `operation` and `description` can be null, but it is recommended to
 * supply the former. See
 * https://develop.sentry.dev/sdk/performance/span-operations/ for conventions
 * around operations.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/span/ for a description of
 * the created Span's properties and expectations for `operation` and
 * `description`.
 *
 * Returns a value that should be passed into `sentry_span_finish`. Not
 * finishing the Span means it will be discarded and will not be sent to
 * sentry. `sentry_value_null` will be returned if the child Span could not be
 * created.
 *
 * To ensure that any Events or Message Events are associated with this
 * Span while it is active, invoke and pass in the Span returned
 * by this function to `sentry_set_span`. Further documentation on this can be
 * found in `sentry_set_span`'s docstring.
 *
 * The returned value is not thread-safe. Users are expected to ensure that
 * appropriate locking mechanisms are implemented over the Span if it needs
 * to be mutated across threads. Methods operating on the Span will mention what
 * kind of expectations they carry if they need to mutate or access the object
 * in a thread-safe way.
 */
SENTRY_EXPERIMENTAL_API sentry_span_t *sentry_span_start_child(
    sentry_span_t *parent, const char *operation, const char *description);
SENTRY_EXPERIMENTAL_API sentry_span_t *sentry_span_start_child_n(
    sentry_span_t *parent, const char *operation, size_t operation_len,
    const char *description, size_t description_len);
/**
 * Also starts a span like the regular `sentry_span_start_child_ts` functions
 * but has an additional timestamp parameter to let the user provide explicit
 * timings.
 *
 * The timestamp must be in microseconds passed since the Unix epoch.
 */
SENTRY_EXPERIMENTAL_API sentry_span_t *sentry_span_start_child_ts(
    sentry_span_t *parent, const char *operation, const char *description,
    uint64_t timestamp);
SENTRY_EXPERIMENTAL_API sentry_span_t *sentry_span_start_child_ts_n(
    sentry_span_t *parent, const char *operation, size_t operation_len,
    const char *description, size_t description_len, uint64_t timestamp);

/**
 * Finishes a Span.
 *
 * This takes ownership of `span`. A Span cannot be modified or re-used after it
 * is finished.
 *
 * This will mutate the `span`'s containing Transaction, so the containing
 * Transaction should also not be mutated by other functions when finishing a
 * span.
 */
SENTRY_EXPERIMENTAL_API void sentry_span_finish(sentry_span_t *span);
/**
 * Also finishes a span like the regular `sentry_span_finish` function but has
 * an additional timestamp parameter to let the user provide explicit timings.
 *
 * The timestamp must be in microseconds passed since the Unix epoch.
 */
SENTRY_EXPERIMENTAL_API void sentry_span_finish_ts(
    sentry_span_t *span, uint64_t timestamp);

/**
 * Sets a tag on a Transaction to the given string value.
 *
 * Tags longer than 200 bytes will be truncated.
 *
 * The Transaction should not be mutated by other functions while a tag is being
 * set on it.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_set_tag(
    sentry_transaction_t *transaction, const char *tag, const char *value);
SENTRY_EXPERIMENTAL_API void sentry_transaction_set_tag_n(
    sentry_transaction_t *transaction, const char *tag, size_t tag_len,
    const char *value, size_t value_len);

/**
 * Removes a tag from a Transaction.
 *
 * The Transaction should not be mutated by other functions while a tag is being
 * removed from it.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_remove_tag(
    sentry_transaction_t *transaction, const char *tag);
SENTRY_EXPERIMENTAL_API void sentry_transaction_remove_tag_n(
    sentry_transaction_t *transaction, const char *tag, size_t tag_len);

/**
 * Sets the given key in a Transaction's "data" section to the given value.
 *
 * The Transaction should not be mutated by other functions while data is being
 * set on it.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_set_data(
    sentry_transaction_t *transaction, const char *key, sentry_value_t value);
SENTRY_EXPERIMENTAL_API void sentry_transaction_set_data_n(
    sentry_transaction_t *transaction, const char *key, size_t key_len,
    sentry_value_t value);

/**
 * Removes a key from a Transaction's "data" section.
 *
 * The Transaction should not be mutated by other functions while data is being
 * removed from it.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_remove_data(
    sentry_transaction_t *transaction, const char *key);
SENTRY_EXPERIMENTAL_API void sentry_transaction_remove_data_n(
    sentry_transaction_t *transaction, const char *key, size_t key_len);

/**
 * Sets a tag on a Span to the given string value.
 *
 * Tags longer than 200 bytes will be truncated.
 *
 * The Span should not be mutated by other functions while a tag is being set on
 * it.
 */
SENTRY_EXPERIMENTAL_API void sentry_span_set_tag(
    sentry_span_t *span, const char *tag, const char *value);
SENTRY_EXPERIMENTAL_API void sentry_span_set_tag_n(sentry_span_t *span,
    const char *tag, size_t tag_len, const char *value, size_t value_len);

/**
 * Removes a tag from a Span.
 *
 * The Span should not be mutated by other functions while a tag is being
 * removed from it.
 */
SENTRY_EXPERIMENTAL_API void sentry_span_remove_tag(
    sentry_span_t *span, const char *tag);
SENTRY_EXPERIMENTAL_API void sentry_span_remove_tag_n(
    sentry_span_t *span, const char *tag, size_t tag_len);

/**
 * Sets the given key in a Span's "data" section to the given value.
 *
 * The Span should not be mutated by other functions while data is being set on
 * it.
 */
SENTRY_EXPERIMENTAL_API void sentry_span_set_data(
    sentry_span_t *span, const char *key, sentry_value_t value);
SENTRY_EXPERIMENTAL_API void sentry_span_set_data_n(
    sentry_span_t *span, const char *key, size_t key_len, sentry_value_t value);

/**
 * Removes a key from a Span's "data" section.
 *
 * The Span should not be mutated by other functions while data is being removed
 * from it.
 */
SENTRY_EXPERIMENTAL_API void sentry_span_remove_data(
    sentry_span_t *span, const char *key);
SENTRY_EXPERIMENTAL_API void sentry_span_remove_data_n(
    sentry_span_t *span, const char *key, size_t key_len);

/**
 * Sets a Transaction's name.
 *
 * The Transaction should not be mutated by other functions while setting its
 * name.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_set_name(
    sentry_transaction_t *transaction, const char *name);
SENTRY_EXPERIMENTAL_API void sentry_transaction_set_name_n(
    sentry_transaction_t *transaction, const char *name, size_t name_len);

/**
 * Creates a deprecated User Report with a specific name, email, and comments.
 *
 * See
 * https://develop.sentry.dev/sdk/data-model/envelope-items/#user-report---deprecated
 */
SENTRY_DEPRECATED("Use `sentry_value_new_feedback` instead")
SENTRY_API sentry_value_t sentry_value_new_user_feedback(
    const sentry_uuid_t *uuid, const char *name, const char *email,
    const char *comments);
SENTRY_DEPRECATED("Use `sentry_value_new_feedback_n` instead")
SENTRY_API sentry_value_t sentry_value_new_user_feedback_n(
    const sentry_uuid_t *uuid, const char *name, size_t name_len,
    const char *email, size_t email_len, const char *comments,
    size_t comments_len);

/**
 * Captures a deprecated User Report and sends it to Sentry.
 */
SENTRY_DEPRECATED("Use `sentry_capture_feedback` instead")
SENTRY_API void sentry_capture_user_feedback(sentry_value_t user_report);

/**
 * Creates a new User Feedback with a specific message (required), and optional
 * contact_email, name, message, and associated_event_id.
 *
 * See https://develop.sentry.dev/sdk/data-model/envelope-items/#user-feedback
 *
 * User Feedback can be associated with a specific event that has been
 * sent to Sentry earlier.
 */
SENTRY_API sentry_value_t sentry_value_new_feedback(const char *message,
    const char *contact_email, const char *name,
    const sentry_uuid_t *associated_event_id);
SENTRY_API sentry_value_t sentry_value_new_feedback_n(const char *message,
    size_t message_len, const char *contact_email, size_t contact_email_len,
    const char *name, size_t name_len,
    const sentry_uuid_t *associated_event_id);

/**
 * Captures a manually created User Feedback and sends it to Sentry.
 */
SENTRY_API void sentry_capture_feedback(sentry_value_t user_feedback);

/**
 * A hint that can be passed to capture functions to provide additional context,
 * such as attachments.
 */
struct sentry_hint_s;
typedef struct sentry_hint_s sentry_hint_t;

/**
 * Creates a new hint to be passed into
 * - `sentry_capture_feedback_with_hint`
 */
SENTRY_API sentry_hint_t *sentry_hint_new(void);

/**
 * Attaches a file to a hint.
 *
 * The file will be read and sent when the event is captured.
 * Returns a pointer to the attachment, or NULL on error.
 */
SENTRY_API sentry_attachment_t *sentry_hint_attach_file(
    sentry_hint_t *hint, const char *path);
SENTRY_API sentry_attachment_t *sentry_hint_attach_file_n(
    sentry_hint_t *hint, const char *path, size_t path_len);

/**
 * Attaches bytes to a hint.
 *
 * The data is copied internally and will be sent when the event is captured.
 * Returns a pointer to the attachment, or NULL on error.
 */
SENTRY_API sentry_attachment_t *sentry_hint_attach_bytes(
    sentry_hint_t *hint, const char *buf, size_t buf_len, const char *filename);
SENTRY_API sentry_attachment_t *sentry_hint_attach_bytes_n(sentry_hint_t *hint,
    const char *buf, size_t buf_len, const char *filename, size_t filename_len);

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Wide char version of `sentry_hint_attach_file`.
 */
SENTRY_API sentry_attachment_t *sentry_hint_attach_filew(
    sentry_hint_t *hint, const wchar_t *path);
SENTRY_API sentry_attachment_t *sentry_hint_attach_filew_n(
    sentry_hint_t *hint, const wchar_t *path, size_t path_len);

/**
 * Wide char version of `sentry_hint_attach_bytes`.
 */
SENTRY_API sentry_attachment_t *sentry_hint_attach_bytesw(sentry_hint_t *hint,
    const char *buf, size_t buf_len, const wchar_t *filename);
SENTRY_API sentry_attachment_t *sentry_hint_attach_bytesw_n(sentry_hint_t *hint,
    const char *buf, size_t buf_len, const wchar_t *filename,
    size_t filename_len);
#endif

/**
 * Captures a manually created feedback with a hint and sends it to Sentry.
 *
 * This function takes ownership of both the feedback value and the hint,
 * which will be freed automatically.
 *
 * The hint parameter can be NULL if no additional context is needed.
 */
SENTRY_API void sentry_capture_feedback_with_hint(
    sentry_value_t user_feedback, sentry_hint_t *hint);

/**
 * The status of a Span or Transaction.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/span/ for documentation.
 */
typedef enum {
    // The operation completed successfully.
    // HTTP status 100..299 + successful redirects from the 3xx range.
    SENTRY_SPAN_STATUS_OK,
    // The operation was cancelled (typically by the user).
    SENTRY_SPAN_STATUS_CANCELLED,
    // Unknown. Any non-standard HTTP status code.
    // "We do not know whether the transaction failed or succeeded."
    SENTRY_SPAN_STATUS_UNKNOWN,
    // Client specified an invalid argument. 4xx.
    // Note that this differs from FailedPrecondition. InvalidArgument
    // indicates arguments that are problematic regardless of the
    // state of the system.
    SENTRY_SPAN_STATUS_INVALID_ARGUMENT,
    // Deadline expired before operation could complete.
    // For operations that change the state of the system, this error may be
    // returned even if the operation has been completed successfully.
    // HTTP redirect loops and 504 Gateway Timeout.
    SENTRY_SPAN_STATUS_DEADLINE_EXCEEDED,
    // 404 Not Found. A requested entity (file or directory) was not found.
    SENTRY_SPAN_STATUS_NOT_FOUND,
    // Already exists (409)
    // Some entity that we attempted to create already exists.
    SENTRY_SPAN_STATUS_ALREADY_EXISTS,
    // 403 Forbidden
    // The caller does not have permission to execute the specified operation.
    SENTRY_SPAN_STATUS_PERMISSION_DENIED,
    // 429 Too Many Requests
    // Some resource has been exhausted, perhaps a per-user quota, or perhaps
    // the entire file system is out of space.
    SENTRY_SPAN_STATUS_RESOURCE_EXHAUSTED,
    // Operation was rejected because the system is not in a state required for
    // the operation's execution.
    SENTRY_SPAN_STATUS_FAILED_PRECONDITION,
    // The operation was aborted, typically due to a concurrency issue.
    SENTRY_SPAN_STATUS_ABORTED,
    // Operation was attempted past the valid range.
    SENTRY_SPAN_STATUS_OUT_OF_RANGE,
    // 501 Not Implemented
    // Operation is not implemented or not enabled.
    SENTRY_SPAN_STATUS_UNIMPLEMENTED,
    // Other/generic 5xx
    SENTRY_SPAN_STATUS_INTERNAL_ERROR,
    // 503 Service Unavailable
    SENTRY_SPAN_STATUS_UNAVAILABLE,
    // Unrecoverable data loss or corruption
    SENTRY_SPAN_STATUS_DATA_LOSS,
    // 401 Unauthorized (actually does mean unauthenticated, according to RFC
    // 7235)
    // Prefer PermissionDenied if a user is logged in.
    SENTRY_SPAN_STATUS_UNAUTHENTICATED,
} sentry_span_status_t;

/**
 * Sets a Span's status.
 *
 * The Span should not be mutated by other functions while setting its status.
 */
SENTRY_EXPERIMENTAL_API void sentry_span_set_status(
    sentry_span_t *span, sentry_span_status_t status);

/**
 * Sets a Transaction's status.
 *
 * The Transaction should not be mutated by other functions while setting its
 * status.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_set_status(
    sentry_transaction_t *tx, sentry_span_status_t status);

/**
 * Type of the `iter_headers` callback.
 *
 * The callback is being called with HTTP header key/value pairs.
 * These headers can be attached to outgoing HTTP requests to propagate
 * distributed tracing metadata to downstream services.
 *
 */
typedef void (*sentry_iter_headers_function_t)(
    const char *key, const char *value, void *userdata);

/**
 * Iterates the distributed tracing HTTP headers for the given span.
 */
SENTRY_EXPERIMENTAL_API void sentry_span_iter_headers(sentry_span_t *span,
    sentry_iter_headers_function_t callback, void *userdata);

/**
 * Iterates the distributed tracing HTTP headers for the given transaction.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_iter_headers(
    sentry_transaction_t *tx, sentry_iter_headers_function_t callback,
    void *userdata);

/**
 * Returns whether the application has crashed on the last run.
 *
 * Notes:
 *   * The underlying value is set by sentry_init() - it must be called first.
 *   * Call sentry_clear_crashed_last_run() to reset for the next app run.
 *
 * Possible return values:
 *   1 = the last run was a crash
 *   0 = no crash recognized
 *  -1 = sentry_init() hasn't been called yet
 */
SENTRY_EXPERIMENTAL_API int sentry_get_crashed_last_run(void);

/**
 * Clear the status of the "crashed-last-run". You should explicitly call
 * this after sentry_init() if you're using sentry_get_crashed_last_run().
 * Otherwise, the same information is reported on any subsequent runs.
 *
 * Notes:
 *   * This doesn't change the value of sentry_get_crashed_last_run() yet.
 *     However, if sentry_init() is called again, the value will change.
 *   * This may only be called after sentry_init() and before sentry_close().
 *
 * Returns 0 on success, 1 on error.
 */
SENTRY_EXPERIMENTAL_API int sentry_clear_crashed_last_run(void);

/**
 * Sentry SDK version.
 */
SENTRY_EXPERIMENTAL_API const char *sentry_sdk_version(void);

/**
 * Sentry SDK name set during build time.
 */
SENTRY_DEPRECATED("Use `sentry_options_get_sdk_name` instead")
SENTRY_EXPERIMENTAL_API const char *sentry_sdk_name(void);

/**
 * Sentry SDK User-Agent set during build time.
 */
SENTRY_DEPRECATED("Use `sentry_options_get_user_agent` instead")
SENTRY_EXPERIMENTAL_API const char *sentry_sdk_user_agent(void);

#ifdef __cplusplus
}
#endif
#endif
