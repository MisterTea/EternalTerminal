#include "sentry_scope.h"
#include "sentry_alloc.h"
#include "sentry_attachment.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_options.h"
#include "sentry_os.h"
#include "sentry_ringbuffer.h"
#include "sentry_string.h"
#include "sentry_symbolizer.h"
#include "sentry_sync.h"
#include "sentry_tracing.h"
#include "sentry_value.h"

#include <stdlib.h>

#ifdef SENTRY_BACKEND_CRASHPAD
#    define SENTRY_BACKEND "crashpad"
#elif defined(SENTRY_BACKEND_BREAKPAD)
#    define SENTRY_BACKEND "breakpad"
#elif defined(SENTRY_BACKEND_INPROC)
#    define SENTRY_BACKEND "inproc"
#elif defined(SENTRY_BACKEND_NATIVE)
#    define SENTRY_BACKEND "native"
#endif

static bool g_scope_initialized = false;
static sentry_scope_t g_scope = { 0 };
#ifdef SENTRY__MUTEX_INIT_DYN
SENTRY__MUTEX_INIT_DYN(g_lock)
#else
static sentry_mutex_t g_lock = SENTRY__MUTEX_INIT;
#endif

static sentry_value_t
get_client_sdk(void)
{
    sentry_value_t client_sdk = sentry_value_new_object();

    // the SDK is not initialized yet, fallback to build-time value
    sentry_value_t sdk_name = sentry_value_new_string(SENTRY_SDK_NAME);
    sentry_value_set_by_key(client_sdk, "name", sdk_name);

    sentry_value_t version = sentry_value_new_string(SENTRY_SDK_VERSION);
    sentry_value_set_by_key(client_sdk, "version", version);

    sentry_value_t package = sentry_value_new_object();

    sentry_value_t package_name
        = sentry_value_new_string("github:getsentry/sentry-native");
    sentry_value_set_by_key(package, "name", package_name);

    sentry_value_incref(version);
    sentry_value_set_by_key(package, "version", version);

    sentry_value_t packages = sentry_value_new_list();
    sentry_value_append(packages, package);
    sentry_value_set_by_key(client_sdk, "packages", packages);

#ifdef SENTRY_BACKEND
    sentry_value_t integrations = sentry_value_new_list();
    sentry_value_append(integrations, sentry_value_new_string(SENTRY_BACKEND));
    sentry_value_set_by_key(client_sdk, "integrations", integrations);
#endif

    return client_sdk;
}

static void
init_scope(sentry_scope_t *scope)
{
    scope->release = NULL;
    scope->environment = NULL;
    scope->transaction = NULL;
    scope->fingerprint = sentry_value_new_null();
    scope->user = sentry_value_new_null();
    scope->tags = sentry_value_new_object();
    scope->extra = sentry_value_new_object();
    scope->attributes = sentry_value_new_object();
    scope->contexts = sentry_value_new_object();
    scope->propagation_context = sentry_value_new_object();
    scope->breadcrumbs = sentry__ringbuffer_new(SENTRY_BREADCRUMBS_MAX);
    scope->dynamic_sampling_context = sentry_value_new_object();
    scope->level = SENTRY_LEVEL_ERROR;
    scope->client_sdk = sentry_value_new_null();
    scope->attachments = NULL;
    scope->transaction_object = NULL;
    scope->span = NULL;
    scope->trace_managed = true;
}

static sentry_scope_t *
get_scope(void)
{
    if (g_scope_initialized) {
        return &g_scope;
    }

    memset(&g_scope, 0, sizeof(sentry_scope_t));
    init_scope(&g_scope);
    g_scope.user = sentry_value_new_object();
    sentry_value_set_by_key(g_scope.contexts, "os", sentry__get_os_context());
    g_scope.client_sdk = get_client_sdk();

    g_scope_initialized = true;

    return &g_scope;
}

static void
cleanup_scope(sentry_scope_t *scope)
{
    sentry_free(scope->release);
    sentry_free(scope->environment);
    sentry_free(scope->transaction);
    sentry_value_decref(scope->fingerprint);
    sentry_value_decref(scope->user);
    sentry_value_decref(scope->tags);
    sentry_value_decref(scope->extra);
    sentry_value_decref(scope->attributes);
    sentry_value_decref(scope->contexts);
    sentry_value_decref(scope->propagation_context);
    sentry__ringbuffer_free(scope->breadcrumbs);
    sentry_value_decref(scope->dynamic_sampling_context);
    sentry_value_decref(scope->client_sdk);
    sentry__attachments_free(scope->attachments);
    sentry__transaction_decref(scope->transaction_object);
    sentry__span_decref(scope->span);
}

void
sentry__scope_cleanup(void)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(g_lock);
    sentry__mutex_lock(&g_lock);
    if (g_scope_initialized) {
        g_scope_initialized = false;
        cleanup_scope(&g_scope);
    }
    sentry__mutex_unlock(&g_lock);
}

sentry_scope_t *
sentry__scope_lock(void)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(g_lock);
    sentry__mutex_lock(&g_lock);
    return get_scope();
}

void
sentry__scope_unlock(void)
{
    SENTRY__MUTEX_INIT_DYN_ONCE(g_lock);
    sentry__mutex_unlock(&g_lock);
}

void
sentry__scope_flush_unlock(void)
{
    sentry__scope_unlock();
    SENTRY_WITH_OPTIONS (options) {
        // we try to unlock the scope as soon as possible. The
        // backend will do its own `WITH_SCOPE` internally.
        if (options->backend && options->backend->flush_scope_func) {
            options->backend->flush_scope_func(options->backend, options);
        }
    }
}

sentry_scope_t *
sentry_local_scope_new(void)
{
    sentry_scope_t *scope = SENTRY_MAKE(sentry_scope_t);
    if (!scope) {
        return NULL;
    }

    init_scope(scope);
    return scope;
}

void
sentry__scope_free(sentry_scope_t *scope)
{
    if (!scope) {
        return;
    }

    cleanup_scope(scope);
    sentry_free(scope);
}

void
sentry__scope_freeze_dsc(sentry_scope_t *scope, sentry_value_t incoming)
{
    sentry_value_decref(scope->dynamic_sampling_context);
    sentry_value_t dsc = sentry_value_new_object();
    sentry__value_merge_objects(dsc, incoming);
    sentry_value_freeze(dsc);
    scope->dynamic_sampling_context = dsc;
}

void
sentry__scope_update_dsc(sentry_scope_t *scope, const sentry_options_t *options)
{
    sentry_value_decref(scope->dynamic_sampling_context);
    sentry_value_t dsc = sentry_value_new_object();

    if (options->dsn) {
        sentry_value_set_by_key(dsc, "public_key",
            sentry_value_new_string(options->dsn->public_key));
    }
    const char *org_id = sentry__options_get_org_id(options);
    if (org_id) {
        sentry_value_set_by_key(dsc, "org_id", sentry_value_new_string(org_id));
    }
    sentry_value_set_by_key(dsc, "sample_rate",
        sentry_value_new_double(options->traces_sample_rate));
    if (options->traces_sampler) {
        sentry_value_set_by_key(
            dsc, "sample_rate", sentry_value_new_double(1.0));
    }
    sentry_value_t sample_rand = sentry_value_get_by_key(
        sentry_value_get_by_key(scope->propagation_context, "trace"),
        "sample_rand");
    sentry_value_set_by_key(dsc, "sample_rand", sample_rand);
    sentry_value_incref(sample_rand);
    sentry_value_set_by_key(
        dsc, "release", sentry_value_new_string(scope->release));
    sentry_value_set_by_key(
        dsc, "environment", sentry_value_new_string(scope->environment));

    scope->dynamic_sampling_context = dsc;
}

#if !defined(SENTRY_PLATFORM_NX)
static void
sentry__foreach_stacktrace(
    sentry_value_t event, void (*func)(sentry_value_t stacktrace))
{
    // We have stacktraces at the following locations:
    // * `exception[.values].X.stacktrace`:
    //   https://develop.sentry.dev/sdk/event-payloads/exception/
    // * `threads[.values].X.stacktrace`:
    //   https://develop.sentry.dev/sdk/event-payloads/threads/

    sentry_value_t exception = sentry_value_get_by_key(event, "exception");
    if (sentry_value_get_type(exception) == SENTRY_VALUE_TYPE_OBJECT) {
        exception = sentry_value_get_by_key(exception, "values");
    }
    if (sentry_value_get_type(exception) == SENTRY_VALUE_TYPE_LIST) {
        size_t len = sentry_value_get_length(exception);
        for (size_t i = 0; i < len; i++) {
            sentry_value_t stacktrace = sentry_value_get_by_key(
                sentry_value_get_by_index(exception, i), "stacktrace");
            if (!sentry_value_is_null(stacktrace)) {
                func(stacktrace);
            }
        }
    }

    sentry_value_t threads = sentry_value_get_by_key(event, "threads");
    if (sentry_value_get_type(threads) == SENTRY_VALUE_TYPE_OBJECT) {
        threads = sentry_value_get_by_key(threads, "values");
    }
    if (sentry_value_get_type(threads) == SENTRY_VALUE_TYPE_LIST) {
        size_t len = sentry_value_get_length(threads);
        for (size_t i = 0; i < len; i++) {
            sentry_value_t stacktrace = sentry_value_get_by_key(
                sentry_value_get_by_index(threads, i), "stacktrace");
            if (!sentry_value_is_null(stacktrace)) {
                func(stacktrace);
            }
        }
    }
}

static void
sentry__symbolize_frame(const sentry_frame_info_t *info, void *data)
{
    // See https://develop.sentry.dev/sdk/event-payloads/stacktrace/
    sentry_value_t frame = *(sentry_value_t *)data;

    if (info->symbol
        && sentry_value_is_null(sentry_value_get_by_key(frame, "function"))) {
        sentry_value_set_by_key(
            frame, "function", sentry_value_new_string(info->symbol));
    }

    if (info->object_name
        && sentry_value_is_null(sentry_value_get_by_key(frame, "package"))) {
        sentry_value_set_by_key(
            frame, "package", sentry_value_new_string(info->object_name));
    }

    if (info->symbol_addr
        && sentry_value_is_null(
            sentry_value_get_by_key(frame, "symbol_addr"))) {
        sentry_value_set_by_key(frame, "symbol_addr",
            sentry__value_new_addr((uint64_t)(size_t)info->symbol_addr));
    }

    if (info->load_addr
        && sentry_value_is_null(sentry_value_get_by_key(frame, "image_addr"))) {
        sentry_value_set_by_key(frame, "image_addr",
            sentry__value_new_addr((uint64_t)(size_t)info->load_addr));
    }
}

static void
sentry__symbolize_stacktrace(sentry_value_t stacktrace)
{
    sentry_value_t frames = sentry_value_get_by_key(stacktrace, "frames");
    if (sentry_value_get_type(frames) != SENTRY_VALUE_TYPE_LIST) {
        return;
    }

    size_t len = sentry_value_get_length(frames);
    for (size_t i = 0; i < len; i++) {
        sentry_value_t frame = sentry_value_get_by_index(frames, i);

        sentry_value_t addr_value
            = sentry_value_get_by_key(frame, "instruction_addr");
        if (sentry_value_is_null(addr_value)) {
            continue;
        }

        // The addr is saved as a hex-number inside the value.
        size_t addr
            = (size_t)strtoll(sentry_value_as_string(addr_value), NULL, 0);
        if (!addr) {
            continue;
        }
        sentry__symbolize((void *)addr, sentry__symbolize_frame, &frame);
    }
}
#endif

static sentry_value_t
get_span_or_transaction(const sentry_scope_t *scope)
{
    if (scope->span) {
        return scope->span->inner;
    } else if (scope->transaction_object) {
        return scope->transaction_object->inner;
    } else {
        return sentry_value_new_null();
    }
}

#ifdef SENTRY_UNITTEST
sentry_value_t
sentry__scope_get_span_or_transaction(void)
{
    sentry_value_t result = sentry_value_new_null();
    SENTRY_WITH_SCOPE (scope) {
        result = get_span_or_transaction(scope);
    }
    return result;
}
#endif

void
sentry__scope_apply_to_event(const sentry_scope_t *scope,
    const sentry_options_t *options, sentry_value_t event,
    sentry_scope_mode_t mode)
{
#define IS_NULL(Key) sentry_value_is_null(sentry_value_get_by_key(event, Key))
#define SET(Key, Value) sentry_value_set_by_key(event, Key, Value)
#define PLACE_STRING(Key, Source)                                              \
    do {                                                                       \
        if (IS_NULL(Key) && !sentry__string_empty(Source)) {                   \
            SET(Key, sentry_value_new_string(Source));                         \
        }                                                                      \
    } while (0)
#define PLACE_VALUE(Key, Source)                                               \
    do {                                                                       \
        if (IS_NULL(Key) && !sentry_value_is_null(Source)) {                   \
            sentry_value_incref(Source);                                       \
            SET(Key, Source);                                                  \
        }                                                                      \
    } while (0)
#define PLACE_CLONED_VALUE(Key, Source)                                        \
    do {                                                                       \
        if (IS_NULL(Key) && !sentry_value_is_null(Source)) {                   \
            SET(Key, sentry__value_clone(Source));                             \
        }                                                                      \
    } while (0)

    PLACE_STRING("platform", "native");

    PLACE_STRING("release", scope->release);
    PLACE_STRING("dist", options->dist);
    PLACE_STRING("environment", scope->environment);

    // is not transaction and has no level
    if (IS_NULL("type") && IS_NULL("level")) {
        SET("level", sentry__value_new_level(scope->level));
    }

    if (sentry_value_get_type(scope->user) == SENTRY_VALUE_TYPE_OBJECT) {
        if (options->run && options->run->installation_id) {
            // ensure event has a user object
            if (IS_NULL("user")) {
                SET("user", sentry__value_clone(scope->user));
            }
            // patch missing user ID with installation ID
            sentry_value_t user = sentry_value_get_by_key(event, "user");
            if (sentry_value_get_type(user) == SENTRY_VALUE_TYPE_OBJECT
                && sentry_value_is_null(sentry_value_get_by_key(user, "id"))) {
                sentry_value_set_by_key(user, "id",
                    sentry_value_new_string(options->run->installation_id));
            }
        } else if (sentry_value_get_length(scope->user) > 0) {
            PLACE_VALUE("user", scope->user);
        }
    }
    PLACE_VALUE("fingerprint", scope->fingerprint);
    PLACE_STRING("transaction", scope->transaction);
    PLACE_VALUE("sdk", scope->client_sdk);

    sentry_value_t event_tags = sentry_value_get_by_key(event, "tags");
    if (sentry_value_is_null(event_tags)) {
        if (!sentry_value_is_null(scope->tags)) {
            PLACE_CLONED_VALUE("tags", scope->tags);
        }
    } else {
        sentry__value_merge_objects(event_tags, scope->tags);
    }
    sentry_value_t event_extra = sentry_value_get_by_key(event, "extra");
    if (sentry_value_is_null(event_extra)) {
        if (!sentry_value_is_null(scope->extra)) {
            PLACE_CLONED_VALUE("extra", scope->extra);
        }
    } else {
        sentry__value_merge_objects(event_extra, scope->extra);
    }

    bool is_transaction = sentry__event_is_transaction(event);
    sentry_value_t contexts = sentry__value_clone(scope->contexts);
    if (is_transaction && !sentry_value_is_null(contexts)) {
        sentry_value_remove_by_key(contexts, "trace");
    }

    // prep contexts sourced from scope; data about transaction on scope needs
    // to be extracted and inserted
    sentry_value_t scoped_txn_or_span = sentry_value_new_null();
    sentry_value_t scope_trace = sentry_value_new_null();
    if (!is_transaction) {
        scoped_txn_or_span = get_span_or_transaction(scope);
        scope_trace = sentry__value_get_trace_context(scoped_txn_or_span);
    }
    if (!sentry_value_is_null(scope_trace)) {
        if (sentry_value_is_null(contexts)) {
            contexts = sentry_value_new_object();
        }
        sentry_value_t scoped_txn_or_span_data
            = sentry_value_get_by_key(scoped_txn_or_span, "data");
        if (!sentry_value_is_null(scoped_txn_or_span_data)) {
            sentry_value_incref(scoped_txn_or_span_data);
            sentry_value_set_by_key(
                scope_trace, "data", scoped_txn_or_span_data);
        }
        sentry_value_set_by_key(contexts, "trace", scope_trace);
    }

    // merge contexts sourced from scope into the event
    sentry_value_t event_contexts = sentry_value_get_by_key(event, "contexts");
    if (sentry_value_is_null(event_contexts)) {
        // only merge in propagation context if there is no scoped span
        if (!is_transaction && sentry_value_is_null(scope_trace)) {
            sentry__value_merge_objects(contexts, scope->propagation_context);
        }
        PLACE_VALUE("contexts", contexts);
    } else {
        sentry__value_merge_objects(event_contexts, contexts);
    }
    sentry_value_decref(contexts);

    if (mode & SENTRY_SCOPE_BREADCRUMBS) {
        sentry_value_t event_breadcrumbs
            = sentry_value_get_by_key(event, "breadcrumbs");
        sentry_value_t scope_breadcrumbs
            = sentry__ringbuffer_to_list(scope->breadcrumbs);
        sentry_value_set_by_key(event, "breadcrumbs",
            sentry__value_merge_breadcrumbs(event_breadcrumbs,
                scope_breadcrumbs, options->max_breadcrumbs));
        sentry_value_decref(scope_breadcrumbs);
    }

#if !defined(SENTRY_PLATFORM_NX)
    if (mode & SENTRY_SCOPE_MODULES) {
        sentry_value_t modules = sentry_get_modules_list();
        if (!sentry_value_is_null(modules)) {
            sentry_value_t debug_meta = sentry_value_new_object();
            sentry_value_set_by_key(debug_meta, "images", modules);
            sentry_value_set_by_key(event, "debug_meta", debug_meta);
        }
    }

    if (mode & SENTRY_SCOPE_STACKTRACES) {
        sentry__foreach_stacktrace(event, sentry__symbolize_stacktrace);
    }
#endif

#undef PLACE_CLONED_VALUE
#undef PLACE_VALUE
#undef PLACE_STRING
#undef SET
#undef IS_NULL
}

void
sentry_scope_add_breadcrumb(sentry_scope_t *scope, sentry_value_t breadcrumb)
{
    sentry__ringbuffer_append(scope->breadcrumbs, breadcrumb);
}

void
sentry_scope_set_user(sentry_scope_t *scope, sentry_value_t user)
{
    sentry_value_decref(scope->user);
    scope->user = user;
}

void
sentry_scope_set_tag(sentry_scope_t *scope, const char *key, const char *value)
{
    sentry_value_set_by_key(scope->tags, key, sentry_value_new_string(value));
}

void
sentry_scope_set_tag_n(sentry_scope_t *scope, const char *key, size_t key_len,
    const char *value, size_t value_len)
{
    sentry_value_set_by_key_n(
        scope->tags, key, key_len, sentry_value_new_string_n(value, value_len));
}

void
sentry_scope_set_extra(
    sentry_scope_t *scope, const char *key, sentry_value_t value)
{
    sentry_value_set_by_key(scope->extra, key, value);
}

void
sentry_scope_set_extra_n(sentry_scope_t *scope, const char *key, size_t key_len,
    sentry_value_t value)
{
    sentry_value_set_by_key_n(scope->extra, key, key_len, value);
}

void
sentry__scope_set_attribute(
    sentry_scope_t *scope, const char *key, sentry_value_t attribute)
{
    sentry__scope_set_attribute_n(scope, key, strlen(key), attribute);
}

void
sentry__scope_set_attribute_n(sentry_scope_t *scope, const char *key,
    size_t key_len, sentry_value_t attribute)
{
    if (sentry_value_is_null(sentry_value_get_by_key(attribute, "value"))
        || sentry_value_is_null(sentry_value_get_by_key(attribute, "type"))) {
        SENTRY_DEBUG("Cannot set attribute with missing 'value' or 'type'");
        return;
    }
    sentry_value_set_by_key_n(scope->attributes, key, key_len, attribute);
}

void
sentry__scope_remove_attribute(sentry_scope_t *scope, const char *key)
{
    sentry_value_remove_by_key(scope->attributes, key);
}

void
sentry__scope_remove_attribute_n(
    sentry_scope_t *scope, const char *key, size_t key_len)
{
    sentry_value_remove_by_key_n(scope->attributes, key, key_len);
}

void
sentry_scope_set_context(
    sentry_scope_t *scope, const char *key, sentry_value_t value)
{
    sentry_value_set_by_key(scope->contexts, key, value);
}

void
sentry_scope_set_context_n(sentry_scope_t *scope, const char *key,
    size_t key_len, sentry_value_t value)
{
    sentry_value_set_by_key_n(scope->contexts, key, key_len, value);
}

void
sentry_scope_update_context(
    sentry_scope_t *scope, const char *key, sentry_value_t value)
{
    sentry_scope_update_context_n(
        scope, key, sentry__guarded_strlen(key), value);
}

void
sentry_scope_update_context_n(sentry_scope_t *scope, const char *key,
    size_t key_len, sentry_value_t value)
{
    sentry_value_t context
        = sentry_value_get_by_key_n(scope->contexts, key, key_len);
    if (sentry_value_is_null(context)) {
        sentry_value_set_by_key_n(scope->contexts, key, key_len, value);
    } else {
        sentry__value_merge_objects(value, context);
        sentry_value_set_by_key_n(scope->contexts, key, key_len, value);
    }
}

void
sentry__scope_set_fingerprint_va(
    sentry_scope_t *scope, const char *fingerprint, va_list va)
{
    sentry_value_t fingerprint_value = sentry_value_new_list();
    for (; fingerprint; fingerprint = va_arg(va, const char *)) {
        sentry_value_append(
            fingerprint_value, sentry_value_new_string(fingerprint));
    }

    sentry_value_decref(scope->fingerprint);
    scope->fingerprint = fingerprint_value;
}

void
sentry__scope_set_fingerprint_nva(sentry_scope_t *scope,
    const char *fingerprint, size_t fingerprint_len, va_list va)
{
    sentry_value_t fingerprint_value = sentry_value_new_list();
    while (fingerprint) {
        sentry_value_append(fingerprint_value,
            sentry_value_new_string_n(fingerprint, fingerprint_len));
        fingerprint = va_arg(va, const char *);
        if (fingerprint) {
            fingerprint_len = va_arg(va, size_t);
        }
    }

    sentry_scope_set_fingerprints(scope, fingerprint_value);
}

void
sentry_scope_set_fingerprint(
    sentry_scope_t *scope, const char *fingerprint, ...)
{
    va_list va;
    va_start(va, fingerprint);

    sentry__scope_set_fingerprint_va(scope, fingerprint, va);

    va_end(va);
}

void
sentry_scope_set_fingerprint_n(
    sentry_scope_t *scope, const char *fingerprint, size_t fingerprint_len, ...)
{
    va_list va;
    va_start(va, fingerprint_len);

    sentry__scope_set_fingerprint_nva(scope, fingerprint, fingerprint_len, va);

    va_end(va);
}

void
sentry_scope_set_fingerprints(
    sentry_scope_t *scope, sentry_value_t fingerprints)
{
    if (sentry_value_get_type(fingerprints) != SENTRY_VALUE_TYPE_LIST) {
        SENTRY_WARN("invalid fingerprints type, expected list");
        return;
    }

    sentry_value_decref(scope->fingerprint);
    scope->fingerprint = fingerprints;
}

void
sentry_scope_set_level(sentry_scope_t *scope, sentry_level_t level)
{
    scope->level = level;
}

sentry_attachment_t *
sentry_scope_attach_file(sentry_scope_t *scope, const char *path)
{
    return sentry_scope_attach_file_n(
        scope, path, sentry__guarded_strlen(path));
}

sentry_attachment_t *
sentry_scope_attach_file_n(
    sentry_scope_t *scope, const char *path, size_t path_len)
{
    return sentry__attachments_add_path(&scope->attachments,
        sentry__path_from_str_n(path, path_len), NULL, NULL);
}

sentry_attachment_t *
sentry_scope_attach_bytes(sentry_scope_t *scope, const char *buf,
    size_t buf_len, const char *filename)
{
    return sentry_scope_attach_bytes_n(
        scope, buf, buf_len, filename, sentry__guarded_strlen(filename));
}

sentry_attachment_t *
sentry_scope_attach_bytes_n(sentry_scope_t *scope, const char *buf,
    size_t buf_len, const char *filename, size_t filename_len)
{
    return sentry__attachments_add(&scope->attachments,
        sentry__attachment_from_buffer(
            buf, buf_len, sentry__path_from_str_n(filename, filename_len)));
}

#ifdef SENTRY_PLATFORM_WINDOWS
sentry_attachment_t *
sentry_scope_attach_filew(sentry_scope_t *scope, const wchar_t *path)
{
    size_t path_len = path ? wcslen(path) : 0;
    return sentry_scope_attach_filew_n(scope, path, path_len);
}

sentry_attachment_t *
sentry_scope_attach_filew_n(
    sentry_scope_t *scope, const wchar_t *path, size_t path_len)
{
    return sentry__attachments_add_path(&scope->attachments,
        sentry__path_from_wstr_n(path, path_len), NULL, NULL);
}

sentry_attachment_t *
sentry_scope_attach_bytesw(sentry_scope_t *scope, const char *buf,
    size_t buf_len, const wchar_t *filename)
{
    size_t filename_len = filename ? wcslen(filename) : 0;
    return sentry_scope_attach_bytesw_n(
        scope, buf, buf_len, filename, filename_len);
}

sentry_attachment_t *
sentry_scope_attach_bytesw_n(sentry_scope_t *scope, const char *buf,
    size_t buf_len, const wchar_t *filename, size_t filename_len)
{
    return sentry__attachments_add(&scope->attachments,
        sentry__attachment_from_buffer(
            buf, buf_len, sentry__path_from_wstr_n(filename, filename_len)));
}
#endif

void
sentry__scope_apply_attributes(const sentry_scope_t *scope,
    sentry_value_t telemetry, sentry_value_t attributes)
{
    sentry__value_merge_objects(attributes, scope->attributes);

    sentry_value_t trace_id = sentry_value_get_by_key(
        sentry_value_get_by_key(scope->propagation_context, "trace"),
        "trace_id");
    sentry_value_incref(trace_id);
    sentry_value_set_by_key(telemetry, "trace_id", trace_id);

    sentry_value_t parent_span_id = sentry_value_new_object();
    sentry_value_t scoped_span_trace_id = sentry_value_new_null();
    if (scope->transaction_object) {
        sentry_value_t span_id = sentry_value_get_by_key(
            scope->transaction_object->inner, "span_id");
        sentry_value_incref(span_id);
        sentry_value_set_by_key(parent_span_id, "value", span_id);
        scoped_span_trace_id = sentry_value_get_by_key(
            scope->transaction_object->inner, "trace_id");
        sentry_value_incref(scoped_span_trace_id);
    } else if (scope->span) {
        sentry_value_t span_id
            = sentry_value_get_by_key(scope->span->inner, "span_id");
        sentry_value_incref(span_id);
        sentry_value_set_by_key(parent_span_id, "value", span_id);
        scoped_span_trace_id
            = sentry_value_get_by_key(scope->span->inner, "trace_id");
        sentry_value_incref(scoped_span_trace_id);
    }
    sentry_value_set_by_key(
        parent_span_id, "type", sentry_value_new_string("string"));
    if (scope->transaction_object || scope->span) {
        sentry_value_set_by_key(
            attributes, "sentry.trace.parent_span_id", parent_span_id);
        sentry_value_set_by_key(telemetry, "trace_id", scoped_span_trace_id);
    } else {
        sentry_value_decref(parent_span_id);
        sentry_value_decref(scoped_span_trace_id);
    }

    if (!sentry_value_is_null(scope->user)) {
        sentry_value_t user_id = sentry_value_get_by_key(scope->user, "id");
        if (!sentry_value_is_null(user_id)) {
            sentry_value_incref(user_id);
            sentry__value_add_attribute(
                attributes, user_id, "string", "user.id");
        }

        sentry_value_t user_username
            = sentry_value_get_by_key(scope->user, "username");
        if (!sentry_value_is_null(user_username)) {
            sentry_value_incref(user_username);
            sentry__value_add_attribute(
                attributes, user_username, "string", "user.name");
        }

        sentry_value_t user_email
            = sentry_value_get_by_key(scope->user, "email");
        if (!sentry_value_is_null(user_email)) {
            sentry_value_incref(user_email);
            sentry__value_add_attribute(
                attributes, user_email, "string", "user.email");
        }
    }
    sentry_value_t os_context = sentry_value_get_by_key(scope->contexts, "os");
    if (!sentry_value_is_null(os_context)) {
        sentry_value_t os_name = sentry_value_get_by_key(os_context, "name");
        sentry_value_t os_version
            = sentry_value_get_by_key(os_context, "version");
        if (!sentry_value_is_null(os_name)) {
            sentry_value_incref(os_name);
            sentry__value_add_attribute(
                attributes, os_name, "string", "os.name");
        }
        if (!sentry_value_is_null(os_version)) {
            sentry_value_incref(os_version);
            sentry__value_add_attribute(
                attributes, os_version, "string", "os.version");
        }
    }
}
