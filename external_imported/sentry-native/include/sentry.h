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
 * encoding, typically ANSI on Windows, UTF-8 macOS, and the locale encoding on
 * Linux; and they provide wchar-compatible alternatives on Windows which are
 * preferred.
 */

#ifndef SENTRY_H_INCLUDED
#define SENTRY_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* SDK Version */
#ifndef SENTRY_SDK_NAME
#    ifdef __ANDROID__
#        define SENTRY_SDK_NAME "sentry.native.android"
#    else
#        define SENTRY_SDK_NAME "sentry.native"
#    endif
#endif
#define SENTRY_SDK_VERSION "0.7.0"
#define SENTRY_SDK_USER_AGENT SENTRY_SDK_NAME "/" SENTRY_SDK_VERSION

/* common platform detection */
#ifdef _WIN32
#    define SENTRY_PLATFORM_WINDOWS
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
#elif defined(__linux) || defined(__linux__)
#    define SENTRY_PLATFORM_LINUX
#    define SENTRY_PLATFORM_UNIX
#elif defined(_AIX)
/* IBM i PASE is also counted as AIX */
#    define SENTRY_PLATFORM_AIX
#    define SENTRY_PLATFORM_UNIX
#else
#    error unsupported platform
#endif

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

/* marks a function as experimental api */
#ifndef SENTRY_EXPERIMENTAL_API
#    define SENTRY_EXPERIMENTAL_API SENTRY_API
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>

/* context type dependencies */
#ifdef _WIN32
#    include <windows.h>
#else
#    include <signal.h>
#endif

/**
 * The library internally uses the system malloc and free functions to manage
 * memory.  It does not use realloc.  The reason for this is that on unix
 * platforms we fall back to a simplistic page allocator once we have
 * encountered a SIGSEGV or other terminating signal as malloc is no longer
 * safe to use.  Since we cannot portably reallocate allocations made on the
 * pre-existing allocator we're instead not using realloc.
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
 * Type of a sentry value.
 */
typedef enum {
    SENTRY_VALUE_TYPE_NULL,
    SENTRY_VALUE_TYPE_BOOL,
    SENTRY_VALUE_TYPE_INT32,
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
 * internal refcount by one.  If the refcount hits zero it's freed.  Some
 * values like primitives have no refcount (like null) so operations on
 * those are no-ops.
 *
 * In addition values can be frozen.  Some values like primitives are always
 * frozen but lists and dicts are not and can be frozen on demand.  This
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
SENTRY_API void sentry_value_incref(sentry_value_t value);

/**
 * Decrements the reference count on the value.
 */
SENTRY_API void sentry_value_decref(sentry_value_t value);

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
 * If the list is shorter than the given index it's automatically extended
 * and filled with `null` values.
 */
SENTRY_API int sentry_value_set_by_index(
    sentry_value_t value, size_t index, sentry_value_t v);

/**
 * This removes a value from the list by index.
 */
SENTRY_API int sentry_value_remove_by_index(sentry_value_t value, size_t index);

/**
 * Looks up a value in a map by key.  If missing a null value is returned.
 * The returned value is borrowed.
 */
SENTRY_API sentry_value_t sentry_value_get_by_key(
    sentry_value_t value, const char *k);
SENTRY_API sentry_value_t sentry_value_get_by_key_n(
    sentry_value_t value, const char *k, size_t k_len);

/**
 * Looks up a value in a map by key.  If missing a null value is returned.
 * The returned value is owned.
 *
 * If the caller no longer needs the value it must be released with
 * `sentry_value_decref`.
 */
SENTRY_API sentry_value_t sentry_value_get_by_key_owned(
    sentry_value_t value, const char *k);
SENTRY_API sentry_value_t sentry_value_get_by_key_owned_n(
    sentry_value_t value, const char *k, size_t k_len);

/**
 * Looks up a value in a list by index.  If missing a null value is returned.
 * The returned value is borrowed.
 */
SENTRY_API sentry_value_t sentry_value_get_by_index(
    sentry_value_t value, size_t index);

/**
 * Looks up a value in a list by index.  If missing a null value is
 * returned. The returned value is owned.
 *
 * If the caller no longer needs the value it must be released with
 * `sentry_value_decref`.
 */
SENTRY_API sentry_value_t sentry_value_get_by_index_owned(
    sentry_value_t value, size_t index);

/**
 * Returns the length of the given map or list.
 *
 * If an item is not a list or map the return value is 0.
 */
SENTRY_API size_t sentry_value_get_length(sentry_value_t value);

/**
 * Converts a value into a 32bit signed integer.
 */
SENTRY_API int32_t sentry_value_as_int32(sentry_value_t value);

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
 * `sentry_string_free`.
 */
SENTRY_API char *sentry_value_to_json(sentry_value_t value);

/**
 * Sentry levels for events and breadcrumbs.
 */
typedef enum sentry_level_e {
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
 * Either parameter can be NULL in which case no such attributes is created.
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
 * If `ips` is NULL the current stack trace is captured, otherwise `len`
 * stack trace instruction pointers are attached to the event.
 */
SENTRY_EXPERIMENTAL_API sentry_value_t sentry_value_new_stacktrace(
    void **ips, size_t len);

/**
 * Sets the Stack Trace conforming to the Stack Trace Interface in a value.
 *
 * The value argument must be either an exception or thread object.
 *
 * If `ips` is NULL the current stack trace is captured, otherwise `len` stack
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
 * `sentry_string_free`.  Since msgpack is not zero terminated
 * the size is written to the `size_out` parameter.
 */
SENTRY_EXPERIMENTAL_API char *sentry_value_to_msgpack(
    sentry_value_t value, size_t *size_out);

/**
 * Adds a stack trace to an event.
 *
 * The stack trace is added as part of a new thread object.
 * This function is **deprecated** in favor of using
 * `sentry_value_new_stacktrace` in combination with `sentry_value_new_thread`
 * and `sentry_event_add_thread`.
 *
 * If `ips` is NULL the current stack trace is captured, otherwise `len`
 * stack trace instruction pointers are attached to the event.
 */
SENTRY_EXPERIMENTAL_API void sentry_event_value_add_stacktrace(
    sentry_value_t event, void **ips, size_t len);

/**
 * This represents the OS dependent user context in the case of a crash, and can
 * be used to manually capture a crash.
 */
typedef struct sentry_ucontext_s {
#ifdef _WIN32
    EXCEPTION_POINTERS exception_ptrs;
#else
    int signum;
    siginfo_t *siginfo;
    ucontext_t *user_context;
#endif
} sentry_ucontext_t;

/**
 * Unwinds the stack from the given address.
 *
 * If the address is given in `addr` the stack is unwound form there.
 * Otherwise (NULL is passed) the current instruction pointer is used as
 * start address.
 * Unwinding with a given `addr` is not supported on all platforms.
 *
 * The stack trace in the form of instruction-addresses, is written to the
 * caller allocated `stacktrace_out`, with up to `max_len` frames being written.
 * The actual number of unwound stackframes is returned.
 */
SENTRY_EXPERIMENTAL_API size_t sentry_unwind_stack(
    void *addr, void **stacktrace_out, size_t max_len);

/**
 * Unwinds the stack from the given context.
 *
 * The caller is responsible to construct an appropriate `sentry_ucontext_t`.
 * Unwinding from a user context is not supported on all platforms.
 *
 * The stack trace in the form of instruction-addresses, is written to the
 * caller allocated `stacktrace_out`, with up to `max_len` frames being written.
 * The actual number of unwound stackframes is returned.
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
 * Creates the nil uuid.
 */
SENTRY_API sentry_uuid_t sentry_uuid_nil(void);

/**
 * Creates a new uuid4.
 */
SENTRY_API sentry_uuid_t sentry_uuid_new_v4(void);

/**
 * Parses a uuid from a string.
 */
SENTRY_API sentry_uuid_t sentry_uuid_from_string(const char *str);
SENTRY_API sentry_uuid_t sentry_uuid_from_string_n(
    const char *str, size_t str_len);

/**
 * Creates a uuid from bytes.
 */
SENTRY_API sentry_uuid_t sentry_uuid_from_bytes(const char bytes[16]);

/**
 * Checks if the uuid is nil.
 */
SENTRY_API int sentry_uuid_is_nil(const sentry_uuid_t *uuid);

/**
 * Returns the bytes of the uuid.
 */
SENTRY_API void sentry_uuid_as_bytes(const sentry_uuid_t *uuid, char bytes[16]);

/**
 * Formats the uuid into a string buffer.
 */
SENTRY_API void sentry_uuid_as_string(const sentry_uuid_t *uuid, char str[37]);

/**
 * A Sentry Envelope.
 *
 * The Envelope is an abstract type which represents a payload being sent to
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
 * The return value needs to be freed with sentry_string_free().
 */
SENTRY_API char *sentry_envelope_serialize(
    const sentry_envelope_t *envelope, size_t *size_out);

/**
 * Serializes the envelope into a file.
 *
 * `path` is assumed to be in platform-specific filesystem path encoding.
 *
 * Returns 0 on success.
 */
SENTRY_API int sentry_envelope_write_to_file(
    const sentry_envelope_t *envelope, const char *path);
SENTRY_API int sentry_envelope_write_to_file_n(
    const sentry_envelope_t *envelope, const char *path, size_t path_len);

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
 * A transport has the following hooks, all of which
 * take the user provided `state` as last parameter. The transport state needs
 * to be set with `sentry_transport_set_state` and typically holds handles and
 * other information that can be reused across requests.
 *
 * * `send_func`: This function will take ownership of an envelope, and is
 *   responsible for freeing it via `sentry_envelope_free`.
 * * `startup_func`: This hook will be called by sentry inside of `sentry_init`
 *   and instructs the transport to initialize itself. Failures will bubble up
 *   to `sentry_init`.
 * * `flush_func`: Instructs the transport to flush its queue.
 *   This hook receives a millisecond-resolution `timeout` parameter and should
 *   return `0` if the transport queue is flushed within the timeout.
 * * `shutdown_func`: Instructs the transport to flush its queue and shut down.
 *   This hook receives a millisecond-resolution `timeout` parameter and should
 *   return `0` if the transport is flushed and shut down successfully.
 *   In case of a non-zero return value, sentry will log an error, but continue
 * with freeing the transport.
 * * `free_func`: Frees the transports `state`. This hook might be called even
 *   though `shutdown_func` returned a failure code previously.
 *
 * The transport interface might be extended in the future with hooks to flush
 * its internal queue without shutting down, and to dump its internal queue to
 * disk in case of a hard crash.
 */
struct sentry_transport_s;
typedef struct sentry_transport_s sentry_transport_t;

/**
 * Creates a new transport with an initial `send_func`.
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
 * options which can be used to initialize a transports internal state.
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
 * Generic way to free a transport.
 */
SENTRY_API void sentry_transport_free(sentry_transport_t *transport);

/**
 * Create a new function transport.
 *
 * It is a convenience function which works with a borrowed `data`, and will
 * automatically free the envelope, so the user provided function does not need
 * to do that.
 *
 * This function is *deprecated* and will be removed in a future version.
 * It is here for backwards compatibility. Users should migrate to the
 * `sentry_transport_new` API.
 */
SENTRY_API sentry_transport_t *sentry_new_function_transport(
    void (*func)(const sentry_envelope_t *envelope, void *data), void *data);

/**
 * This represents an interface for user-defined backends.
 *
 * Backends are responsible to handle crashes. They are maintained at runtime
 * via various life-cycle hooks from the sentry-core.
 *
 * At this point none of those interfaces are exposed in the API including
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

/**
 * Type of the `before_send` callback.
 *
 * The callback takes ownership of the `event`, and should usually return that
 * same event. In case the event should be discarded, the callback needs to
 * call `sentry_value_decref` on the provided event, and return a
 * `sentry_value_new_null()` instead.
 *
 * If you have set an `on_crash` callback (independent of whether it discards or
 * retains the event), `before_send` will no longer be invoked for crash-events,
 * which allows you to better distinguish between crashes and all other events
 * in client-side pre-processing.
 *
 * This function may be invoked inside of a signal handler and must be safe for
 * that purpose, see https://man7.org/linux/man-pages/man7/signal-safety.7.html.
 * On Windows, it may be called from inside of a `UnhandledExceptionFilter`, see
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
 * On Windows the crashpad backend can capture fast-fail crashes which by-pass
 * SEH. Since the `before_send` is called by a local exception-handler, it will
 * not be invoked when such a crash happened, even though a minidump will be
 * sent.
 */
typedef sentry_value_t (*sentry_event_function_t)(
    sentry_value_t event, void *hint, void *closure);

/**
 * Sets the `before_send` callback.
 *
 * See the `sentry_event_function_t` typedef above for more information.
 */
SENTRY_API void sentry_options_set_before_send(
    sentry_options_t *opts, sentry_event_function_t func, void *data);

/**
 * Type of the `on_crash` callback.
 *
 * The `on_crash` callback replaces the `before_send` callback for crash events.
 * The interface is analogous to `before_send` in that the callback takes
 * ownership of the `event`, and should usually return that same event. In case
 * the event should be discarded, the callback needs to call
 * `sentry_value_decref` on the provided event, and return a
 * `sentry_value_new_null()` instead.
 *
 * Only the `inproc` backend currently fills the passed-in event with useful
 * data and processes any modifications to the return value. Since both
 * `breakpad` and `crashpad` use minidumps to capture the crash state, the
 * passed-in event is empty when using these backends, and they ignore any
 * changes to the return value.
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
 * This function may be invoked inside of a signal handler and must be safe for
 * that purpose, see https://man7.org/linux/man-pages/man7/signal-safety.7.html.
 * On Windows, it may be called from inside of a `UnhandledExceptionFilter`, see
 * the documentation on SEH (structured exception handling) for more information
 * https://docs.microsoft.com/en-us/windows/win32/debug/structured-exception-handling
 *
 * Platform-specific behavior:
 *
 *  - does not work with crashpad on macOS.
 *  - for breakpad on Linux the `uctx` parameter is always NULL.
 *  - on Windows the crashpad backend can capture fast-fail crashes which
 * by-pass SEH. Since `on_crash` is called by a local exception-handler, it will
 * not be invoked when such a crash happened, even though a minidump will be
 * sent.
 */
typedef sentry_value_t (*sentry_crash_function_t)(
    const sentry_ucontext_t *uctx, sentry_value_t event, void *closure);

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
 * Configures the http proxy.
 *
 * The given proxy has to include the full scheme, eg. `http://some.proxy/`.
 */
SENTRY_API void sentry_options_set_http_proxy(
    sentry_options_t *opts, const char *proxy);
SENTRY_API void sentry_options_set_http_proxy_n(
    sentry_options_t *opts, const char *proxy, size_t proxy_len);

/**
 * Returns the configured http proxy.
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
 * Returns the configured sentry SDK name. Unless overwritten this defaults to
 * SENTRY_SDK_NAME.
 */
SENTRY_API const char *sentry_options_get_sdk_name(
    const sentry_options_t *opts);

/**
 * Returns the user agent. Unless overwritten this defaults to
 * "SENTRY_SDK_NAME / SENTRY_SDK_VERSION".
 */
SENTRY_API const char *sentry_options_get_user_agent(
    const sentry_options_t *opts);

/**
 * Enables or disables debug printing mode.
 */
SENTRY_API void sentry_options_set_debug(sentry_options_t *opts, int debug);

/**
 * Returns the current value of the debug flag.
 */
SENTRY_API int sentry_options_get_debug(const sentry_options_t *opts);

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
 * Type of the callback for logger function.
 */
typedef void (*sentry_logger_function_t)(
    sentry_level_t level, const char *message, va_list args, void *userdata);

/**
 * Sets the sentry-native logger function.
 *
 * Used for logging debug events when the `debug` option is set to true.
 */
SENTRY_API void sentry_options_set_logger(
    sentry_options_t *opts, sentry_logger_function_t func, void *userdata);

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
 * This feature can have a performance impact, and is enabled by default on
 * Android. It is usually only needed when it is not possible to provide debug
 * information files for system libraries which are needed for serverside
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
 * Adds a new attachment to be sent along.
 *
 * `path` is assumed to be in platform-specific filesystem path encoding.
 * API Users on windows are encouraged to use `sentry_options_add_attachmentw`
 * instead.
 */
SENTRY_API void sentry_options_add_attachment(
    sentry_options_t *opts, const char *path);
SENTRY_API void sentry_options_add_attachment_n(
    sentry_options_t *opts, const char *path, size_t path_len);

/**
 * Sets the path to the crashpad handler if the crashpad backend is used.
 *
 * The path defaults to the `crashpad_handler`/`crashpad_handler.exe`
 * executable, depending on platform, which is expected to be present in the
 * same directory as the app executable.
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
 * exist, and will be resolved to an absolute path inside of `sentry_init`. The
 * directory should not be shared with other application data/configuration, as
 * sentry-native will enumerate and possibly delete files in that directory. An
 * example might be `$XDG_CACHE_HOME/your-app/sentry`
 *
 * If no explicit path it set, sentry-native will default to `.sentry-native` in
 * the current working directory, with no specific platform-specific handling.
 *
 * `path` is assumed to be in platform-specific filesystem path encoding.
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
 * Wide char version of `sentry_options_set_handler_path`.
 */
SENTRY_API void sentry_options_set_handler_pathw(
    sentry_options_t *opts, const wchar_t *path);
SENTRY_API void sentry_options_set_handler_pathw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len);

/**
 * Wide char version of `sentry_options_set_database_path`.
 */
SENTRY_API void sentry_options_set_database_pathw(
    sentry_options_t *opts, const wchar_t *path);
SENTRY_API void sentry_options_set_database_pathw_n(
    sentry_options_t *opts, const wchar_t *path, size_t path_len);
#endif

/**
 * Enables forwarding to the system crash reporter. Disabled by default.
 *
 * This setting only has an effect when using Crashpad on macOS. If enabled,
 * Crashpad forwards crashes to the macOS system crash reporter. Depending
 * on the crash, this may impact the crash time. Even if enabled, Crashpad
 * may choose not to forward certain crashes.
 */
SENTRY_API void sentry_options_set_system_crash_reporter_enabled(
    sentry_options_t *opts, int enabled);

/**
 * Sets the maximum time (in milliseconds) to wait for the asynchronous tasks to
 * end on shutdown, before attempting a forced termination.
 */
SENTRY_API void sentry_options_set_shutdown_timeout(
    sentry_options_t *opts, uint64_t shutdown_timeout);

/**
 * Gets the maximum time (in milliseconds) to wait for the asynchronous tasks to
 * end on shutdown, before attempting a forced termination.
 */
SENTRY_API uint64_t sentry_options_get_shutdown_timeout(sentry_options_t *opts);

/**
 * Sets a user-defined backend.
 *
 * Since creation and destruction of backends is not exposed in the API, this
 * can only be used to set the backend to `NULL`, which disables the backend in
 * the initialization.
 */
SENTRY_API void sentry_options_set_backend(
    sentry_options_t *opts, sentry_backend_t *backend);

/* -- Global APIs -- */

/**
 * Initializes the Sentry SDK with the specified options.
 *
 * This takes ownership of the options.  After the options have been set
 * they cannot be modified any more.
 * Depending on the configured transport and backend, this function might not be
 * fully thread-safe.
 * Returns 0 on success.
 */
SENTRY_API int sentry_init(sentry_options_t *options);

/**
 * Instructs the transport to flush its send queue.
 *
 * The `timeout` parameter is in milliseconds.
 *
 * Returns 0 on success, or a non-zero return value in case the timeout is hit.
 *
 * Note that this function will block the thread it was called from until the
 * sentry background worker has finished its work or it timed out, whichever
 * comes first.
 */
SENTRY_API int sentry_flush(uint64_t timeout);

/**
 * Shuts down the sentry client and forces transports to flush out.
 *
 * Returns 0 on success.
 *
 * Note that this does not uninstall any crash handler installed by our
 * backends, which will still process crashes after `sentry_close()`, except
 * when using `crashpad` on Linux or the `inproc` backend.
 *
 * Further note that this function will block the thread it was called from
 * until the sentry background worker has finished its work or it timed out,
 * whichever comes first.
 */
SENTRY_API int sentry_close(void);

/**
 * Shuts down the sentry client and forces transports to flush out.
 *
 * This is a **deprecated** alias for `sentry_close`.
 *
 * Returns 0 on success.
 */
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
 * `sentry_clear_modulecache` when doing so, to make sure that the next call to
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
 * Sends a sentry event.
 *
 * If returns a nil UUID if the event being passed in is a transaction, and the
 * transaction will not be sent nor consumed. `sentry_transaction_finish` should
 * be used to send transactions.
 */
SENTRY_API sentry_uuid_t sentry_capture_event(sentry_value_t event);

/**
 * Captures an exception to be handled by the backend.
 *
 * This is safe to be called from a crashing thread and may not return.
 */
SENTRY_EXPERIMENTAL_API void sentry_handle_exception(
    const sentry_ucontext_t *uctx);

/**
 * Adds the breadcrumb to be sent in case of an event.
 */
SENTRY_API void sentry_add_breadcrumb(sentry_value_t breadcrumb);

/**
 * Sets the specified user.
 */
SENTRY_API void sentry_set_user(sentry_value_t user);

/**
 * Removes a user.
 */
SENTRY_API void sentry_remove_user(void);

/**
 * Sets a tag.
 */
SENTRY_API void sentry_set_tag(const char *key, const char *value);
SENTRY_API void sentry_set_tag_n(
    const char *key, size_t key_len, const char *value, size_t value_len);

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

/**
 * Removes the extra with the specified key.
 */
SENTRY_API void sentry_remove_extra(const char *key);
SENTRY_API void sentry_remove_extra_n(const char *key, size_t key_len);

/**
 * Sets a context object.
 */
SENTRY_API void sentry_set_context(const char *key, sentry_value_t value);
SENTRY_API void sentry_set_context_n(
    const char *key, size_t key_len, sentry_value_t value);

/**
 * Removes the context object with the specified key.
 */
SENTRY_API void sentry_remove_context(const char *key);
SENTRY_API void sentry_remove_context_n(const char *key, size_t key_len);

/**
 * Sets the event fingerprint.
 *
 * This accepts a variable number of arguments, and needs to be terminated by a
 * trailing `NULL`.
 */
SENTRY_API void sentry_set_fingerprint(const char *fingerprint, ...);
SENTRY_API void sentry_set_fingerprint_n(
    const char *fingerprint, size_t fingerprint_len, ...);

/**
 * Removes the fingerprint.
 */
SENTRY_API void sentry_remove_fingerprint(void);

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
 * A sentry Transaction Context.
 *
 * See Transaction Interface under
 * https://develop.sentry.dev/sdk/performance/#new-span-and-transaction-classes
 */
struct sentry_transaction_context_s;
typedef struct sentry_transaction_context_s sentry_transaction_context_t;

/**
 * A sentry Transaction.
 *
 * See https://develop.sentry.dev/sdk/event-payloads/transaction/
 */
struct sentry_transaction_s;
typedef struct sentry_transaction_s sentry_transaction_t;

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
    sentry_transaction_context_t *tx_cxt, const char *name);
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_set_name_n(
    sentry_transaction_context_t *tx_cxt, const char *name, size_t name_len);

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
    sentry_transaction_context_t *tx_cxt, const char *operation);
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_set_operation_n(
    sentry_transaction_context_t *tx_cxt, const char *operation,
    size_t operation_len);

/**
 * Sets the `sampled` field on a Transaction Context, which will be used in the
 * Transaction constructed off of the context.
 *
 * When passed any value above 0, the Transaction will bypass all sampling
 * options and always be sent to sentry. If passed 0, this Transaction and its
 * child spans will never be sent to sentry.
 *
 * The Transaction Context should not be mutated by other functions while
 * setting `sampled` on it.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_set_sampled(
    sentry_transaction_context_t *tx_cxt, int sampled);

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
    sentry_transaction_context_t *tx_cxt);

/**
 * Update the Transaction Context with the given HTTP header key/value pair.
 *
 * This is used to propagate distributed tracing metadata from upstream
 * services. Therefore, the headers of incoming requests should be fed into this
 * function so that sentry is able to continue a trace that was started by an
 * upstream service.
 */
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_update_from_header(
    sentry_transaction_context_t *tx_cxt, const char *key, const char *value);
SENTRY_EXPERIMENTAL_API void sentry_transaction_context_update_from_header_n(
    sentry_transaction_context_t *tx_cxt, const char *key, size_t key_len,
    const char *value, size_t value_len);

/**
 * Starts a new Transaction based on the provided context, restored from an
 * external integration (i.e. a span from a different SDK) or manually
 * constructed by a user.
 *
 * The second parameter is a custom Sampling Context to be used with a Traces
 * Sampler to make a more informed sampling decision. The SDK does not currently
 * support a custom Traces Sampler and this parameter is ignored for the time
 * being but needs to be provided.
 *
 * Returns a Transaction, which is expected to be manually managed by the
 * caller. Manual management involves ensuring that `sentry_transaction_finish`
 * is invoked for the Transaction, and that the caller manually starts and
 * finishes any child Spans as needed on the Transaction.
 *
 * Not invoking `sentry_transaction_finish` with the returned Transaction means
 * it will be discarded, and will not be sent to sentry.
 *
 * To ensure that any Events or Message Events are associated with this
 * Transaction while it is active, invoke and pass in the Transaction returned
 * by this function to `sentry_set_transaction_object`. Further documentation on
 * this can be found in `sentry_set_transaction_object`'s docstring.
 *
 * Takes ownership of `transaction_context`. A Transaction Context cannot be
 * modified or re-used after it is used to start a Transaction.
 *
 * The returned value is not thread-safe. Users are expected to ensure that
 * appropriate locking mechanisms are implemented over the Transaction if it
 * needs to be mutated across threads. Methods operating on the Transaction will
 * mention what kind of expectations they carry if they need to mutate or access
 * the object in a thread-safe way.
 */
SENTRY_EXPERIMENTAL_API sentry_transaction_t *sentry_transaction_start(
    sentry_transaction_context_t *tx_cxt, sentry_value_t sampling_ctx);

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
 * `parent`.
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
 * finishing the Span means it will be discarded, and will not be sent to
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
 * Starts a new Span.
 *
 * The return value of `sentry_span_start_child` may be passed in as `parent`.
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
 * finishing the Span means it will be discarded, and will not be sent to
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
    // 404 Not Found. Some requested entity (file or directory) was not found.
    SENTRY_SPAN_STATUS_NOT_FOUND,
    // Already exists (409)
    // Some entity that we attempted to create already exists.
    SENTRY_SPAN_STATUS_ALREADY_EXISTS,
    // 403 Forbidden
    // The caller does not have permission to execute the specified operation.
    SENTRY_SPAN_STATUS_PERMISSION_DENIED,
    // 429 Too Many Requests
    // Some resource has been exhausted, perhaps a per-user quota or perhaps
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
    // 401 Unauthorized (actually does mean unauthenticated according to RFC
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
 * Deprecated: Please use sentry_options_get_sdk_name instead.
 */
SENTRY_EXPERIMENTAL_API const char *sentry_sdk_name(void);

/**
 * Sentry SDK User-Agent set during build time.
 * Deprecated: Please use sentry_options_get_user_agent instead.
 */
SENTRY_EXPERIMENTAL_API const char *sentry_sdk_user_agent(void);

#ifdef __cplusplus
}
#endif
#endif
