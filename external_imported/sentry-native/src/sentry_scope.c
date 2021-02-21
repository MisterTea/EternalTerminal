#include "sentry_scope.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_database.h"
#include "sentry_options.h"
#include "sentry_os.h"
#include "sentry_string.h"
#include "sentry_symbolizer.h"
#include "sentry_sync.h"
#include <stdlib.h>

#ifdef SENTRY_BACKEND_CRASHPAD
#    define SENTRY_BACKEND "crashpad"
#elif defined(SENTRY_BACKEND_BREAKPAD)
#    define SENTRY_BACKEND "breakpad"
#elif defined(SENTRY_BACKEND_INPROC)
#    define SENTRY_BACKEND "inproc"
#endif

static bool g_scope_initialized = false;
static sentry_scope_t g_scope = { 0 };
static sentry_mutex_t g_lock = SENTRY__MUTEX_INIT;

static sentry_value_t
get_client_sdk(void)
{
    sentry_value_t client_sdk = sentry_value_new_object();

    sentry_value_t name = sentry_value_new_string(SENTRY_SDK_NAME);
    sentry_value_set_by_key(client_sdk, "name", name);

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

    sentry_value_freeze(client_sdk);
    return client_sdk;
}

static sentry_scope_t *
get_scope(void)
{
    if (g_scope_initialized) {
        return &g_scope;
    }

    memset(&g_scope, 0, sizeof(sentry_scope_t));
    g_scope.transaction = NULL;
    g_scope.fingerprint = sentry_value_new_null();
    g_scope.user = sentry_value_new_null();
    g_scope.tags = sentry_value_new_object();
    g_scope.extra = sentry_value_new_object();
    g_scope.contexts = sentry_value_new_object();
    sentry_value_set_by_key(g_scope.contexts, "os", sentry__get_os_context());
    g_scope.breadcrumbs = sentry_value_new_list();
    g_scope.level = SENTRY_LEVEL_ERROR;
    g_scope.client_sdk = get_client_sdk();
    g_scope.session = NULL;

    g_scope_initialized = true;

    return &g_scope;
}

void
sentry__scope_cleanup(void)
{
    sentry__mutex_lock(&g_lock);
    if (g_scope_initialized) {
        g_scope_initialized = false;
        sentry_free(g_scope.transaction);
        sentry_value_decref(g_scope.fingerprint);
        sentry_value_decref(g_scope.user);
        sentry_value_decref(g_scope.tags);
        sentry_value_decref(g_scope.extra);
        sentry_value_decref(g_scope.contexts);
        sentry_value_decref(g_scope.breadcrumbs);
        sentry_value_decref(g_scope.client_sdk);
    }
    sentry__mutex_unlock(&g_lock);
}

sentry_scope_t *
sentry__scope_lock(void)
{
    sentry__mutex_lock(&g_lock);
    return get_scope();
}

void
sentry__scope_unlock(void)
{
    sentry__mutex_unlock(&g_lock);
}

void
sentry__scope_flush_unlock(const sentry_scope_t *scope)
{
    bool did_unlock = false;
    SENTRY_WITH_OPTIONS (options) {
        if (scope->session) {
            sentry__run_write_session(options->run, scope->session);
            sentry__scope_unlock();
        } else {
            sentry__scope_unlock();
            sentry__run_clear_session(options->run);
        }
        did_unlock = true;
        // we try to unlock the scope/session lock as soon as possible. The
        // backend will do its own `WITH_SCOPE` internally.
        if (options->backend && options->backend->flush_scope_func) {
            options->backend->flush_scope_func(options->backend);
        }
    }
    if (!did_unlock) {
        sentry__scope_unlock();
    }
}

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

void
sentry__scope_apply_to_event(
    const sentry_scope_t *scope, sentry_value_t event, sentry_scope_mode_t mode)
{
#define IS_NULL(Key) sentry_value_is_null(sentry_value_get_by_key(event, Key))
#define SET(Key, Value) sentry_value_set_by_key(event, Key, Value)
#define PLACE_STRING(Key, Source)                                              \
    do {                                                                       \
        if (IS_NULL(Key) && Source && *Source) {                               \
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

    SENTRY_WITH_OPTIONS (options) {
        PLACE_STRING("release", options->release);
        PLACE_STRING("dist", options->dist);
        PLACE_STRING("environment", options->environment);
    }

    if (IS_NULL("level")) {
        SET("level", sentry__value_new_level(scope->level));
    }

    PLACE_VALUE("user", scope->user);
    PLACE_VALUE("fingerprint", scope->fingerprint);
    PLACE_STRING("transaction", scope->transaction);
    PLACE_VALUE("sdk", scope->client_sdk);

    // TODO: these should merge
    PLACE_CLONED_VALUE("tags", scope->tags);
    PLACE_CLONED_VALUE("extra", scope->extra);
    PLACE_CLONED_VALUE("contexts", scope->contexts);

    if (mode & SENTRY_SCOPE_BREADCRUMBS) {
        PLACE_CLONED_VALUE("breadcrumbs", scope->breadcrumbs);
    }

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

#undef PLACE_STRING
#undef IS_NULL
#undef SET
}

void
sentry__scope_session_sync(sentry_scope_t *scope)
{
    if (!scope->session) {
        return;
    }

    if (!sentry_value_is_null(scope->user)) {
        sentry_value_t did = sentry_value_get_by_key(scope->user, "id");
        if (sentry_value_is_null(did)) {
            did = sentry_value_get_by_key(scope->user, "email");
        }
        if (sentry_value_is_null(did)) {
            did = sentry_value_get_by_key(scope->user, "username");
        }
        sentry_value_decref(scope->session->distinct_id);
        sentry_value_incref(did);
        scope->session->distinct_id = did;
    }
}
