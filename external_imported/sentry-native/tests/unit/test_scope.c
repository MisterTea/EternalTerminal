#include "sentry.h"
#include "sentry_database.h"
#include "sentry_options.h"
#include "sentry_scope.h"
#include "sentry_testsupport.h"
#include "sentry_utils.h"

SENTRY_TEST(scope_contexts)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

#define TEST_CHECK_CONTEXT_EQUAL(event, key, value)                            \
    do {                                                                       \
        sentry_value_t contexts = sentry_value_get_by_key(event, "contexts");  \
        TEST_CHECK_STRING_EQUAL(                                               \
            sentry_value_as_string(sentry_value_get_by_key(contexts, key)),    \
            value);                                                            \
    } while (0)

    // global:
    // {"all":"global","scope":"global","global":"global"}
    sentry_set_context("all", sentry_value_new_string("global"));
    sentry_set_context("global", sentry_value_new_string("global"));
    sentry_set_context("scope", sentry_value_new_string("global"));

    SENTRY_WITH_SCOPE (global_scope) {
        // event:
        // {"all":"event","event":"event"}
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t contexts = sentry_value_new_object();
            sentry_value_set_by_key(
                contexts, "all", sentry_value_new_string("event"));
            sentry_value_set_by_key(
                contexts, "event", sentry_value_new_string("event"));
            sentry_value_set_by_key(event, "contexts", contexts);
        }

        // event <- global:
        // {"all":"event","event":"event","global":"global","scope":"global"}
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_CONTEXT_EQUAL(event, "all", "event");
        TEST_CHECK_CONTEXT_EQUAL(event, "event", "event");
        TEST_CHECK_CONTEXT_EQUAL(event, "global", "global");
        TEST_CHECK_CONTEXT_EQUAL(event, "scope", "global");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // local:
        // {"all":"scope","scope":"scope","local":"local"}
        sentry_scope_t *local_scope = sentry_local_scope_new();
        sentry_scope_set_context(
            local_scope, "all", sentry_value_new_string("local"));
        sentry_scope_set_context(
            local_scope, "local", sentry_value_new_string("local"));
        sentry_scope_set_context(
            local_scope, "scope", sentry_value_new_string("local"));

        // event:
        // {"all":"event","event":"event"}
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t contexts = sentry_value_new_object();
            sentry_value_set_by_key(
                contexts, "all", sentry_value_new_string("event"));
            sentry_value_set_by_key(
                contexts, "event", sentry_value_new_string("event"));
            sentry_value_set_by_key(event, "contexts", contexts);
        }

        // event <- local:
        // {"all":"event","event":"event","local":"local","scope":"local"}
        sentry__scope_apply_to_event(
            local_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_CONTEXT_EQUAL(event, "all", "event");
        TEST_CHECK_CONTEXT_EQUAL(event, "event", "event");
        TEST_CHECK_CONTEXT_EQUAL(event, "local", "local");
        TEST_CHECK_CONTEXT_EQUAL(event, "scope", "local");

        // event <- global:
        // {"all":"event","event":"event","global":"global","local":"local","scope":"local"}
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_CONTEXT_EQUAL(event, "all", "event");
        TEST_CHECK_CONTEXT_EQUAL(event, "event", "event");
        TEST_CHECK_CONTEXT_EQUAL(event, "global", "global");
        TEST_CHECK_CONTEXT_EQUAL(event, "local", "local");
        TEST_CHECK_CONTEXT_EQUAL(event, "scope", "local");

        sentry__scope_free(local_scope);
        sentry_value_decref(event);
    }

#undef TEST_CHECK_CONTEXT_EQUAL

    sentry_close();
}

SENTRY_TEST(scope_update_context)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    // update into empty: context does not exist yet
    {
        sentry_value_t device = sentry_value_new_object();
        sentry_value_set_by_key(
            device, "model", sentry_value_new_string("Xbox Series X"));
        sentry_value_set_by_key(
            device, "family", sentry_value_new_string("Xbox"));
        sentry_update_context("device", device);

        SENTRY_WITH_SCOPE (scope) {
            sentry_value_t ctx
                = sentry_value_get_by_key(scope->contexts, "device");
            TEST_CHECK_STRING_EQUAL(
                sentry_value_as_string(sentry_value_get_by_key(ctx, "model")),
                "Xbox Series X");
            TEST_CHECK_STRING_EQUAL(
                sentry_value_as_string(sentry_value_get_by_key(ctx, "family")),
                "Xbox");
        }
    }

    // update should overwrite existing keys
    {
        sentry_value_t extra = sentry_value_new_object();
        sentry_value_set_by_key(extra, "model", sentry_value_new_string("PC"));
        sentry_value_set_by_key(
            extra, "cpu_description", sentry_value_new_string("some cpu"));
        sentry_update_context("device", extra);

        SENTRY_WITH_SCOPE (scope) {
            sentry_value_t ctx
                = sentry_value_get_by_key(scope->contexts, "device");
            TEST_CHECK_STRING_EQUAL(
                sentry_value_as_string(sentry_value_get_by_key(ctx, "model")),
                "PC");
            TEST_CHECK_STRING_EQUAL(
                sentry_value_as_string(sentry_value_get_by_key(ctx, "family")),
                "Xbox");
            TEST_CHECK_STRING_EQUAL(
                sentry_value_as_string(
                    sentry_value_get_by_key(ctx, "cpu_description")),
                "some cpu");
        }
    }

    // scoped update into empty
    {
        sentry_scope_t *local_scope = sentry_local_scope_new();

        sentry_value_t os = sentry_value_new_object();
        sentry_value_set_by_key(os, "name", sentry_value_new_string("SteamOS"));
        sentry_scope_update_context(local_scope, "os", os);

        sentry_value_t ctx
            = sentry_value_get_by_key(local_scope->contexts, "os");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(ctx, "name")),
            "SteamOS");

        // scoped update overwrites existing keys
        sentry_value_t os2 = sentry_value_new_object();
        sentry_value_set_by_key(os2, "name", sentry_value_new_string("Linux"));
        sentry_value_set_by_key(os2, "version", sentry_value_new_string("6.1"));
        sentry_scope_update_context(local_scope, "os", os2);

        ctx = sentry_value_get_by_key(local_scope->contexts, "os");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(ctx, "name")),
            "Linux");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(ctx, "version")),
            "6.1");

        sentry__scope_free(local_scope);
    }

    sentry_close();
}

SENTRY_TEST(scope_extra)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

#define TEST_CHECK_EXTRA_EQUAL(event, key, value)                              \
    do {                                                                       \
        sentry_value_t extra = sentry_value_get_by_key(event, "extra");        \
        TEST_CHECK_STRING_EQUAL(                                               \
            sentry_value_as_string(sentry_value_get_by_key(extra, key)),       \
            value);                                                            \
    } while (0)

    // global:
    // {"all":"global","scope":"global","global":"global"}
    sentry_set_extra("all", sentry_value_new_string("global"));
    sentry_set_extra("global", sentry_value_new_string("global"));
    sentry_set_extra("scope", sentry_value_new_string("global"));

    SENTRY_WITH_SCOPE (global_scope) {
        // event:
        // {"all":"event","event":"event"}
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t extra = sentry_value_new_object();
            sentry_value_set_by_key(
                extra, "all", sentry_value_new_string("event"));
            sentry_value_set_by_key(
                extra, "event", sentry_value_new_string("event"));
            sentry_value_set_by_key(event, "extra", extra);
        }

        // event <- global:
        // {"all":"event","event":"event","global":"global","scope":"global"}
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_EXTRA_EQUAL(event, "all", "event");
        TEST_CHECK_EXTRA_EQUAL(event, "event", "event");
        TEST_CHECK_EXTRA_EQUAL(event, "global", "global");
        TEST_CHECK_EXTRA_EQUAL(event, "scope", "global");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // local:
        // {"all":"scope","scope":"scope","local":"local"}
        sentry_scope_t *local_scope = sentry_local_scope_new();
        sentry_scope_set_extra(
            local_scope, "all", sentry_value_new_string("local"));
        sentry_scope_set_extra(
            local_scope, "local", sentry_value_new_string("local"));
        sentry_scope_set_extra(
            local_scope, "scope", sentry_value_new_string("local"));

        // event:
        // {"all":"event","event":"event"}
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t extra = sentry_value_new_object();
            sentry_value_set_by_key(
                extra, "all", sentry_value_new_string("event"));
            sentry_value_set_by_key(
                extra, "event", sentry_value_new_string("event"));
            sentry_value_set_by_key(event, "extra", extra);
        }

        // event <- local:
        // {"all":"event","event":"event","local":"local","scope":"local"}
        sentry__scope_apply_to_event(
            local_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_EXTRA_EQUAL(event, "all", "event");
        TEST_CHECK_EXTRA_EQUAL(event, "event", "event");
        TEST_CHECK_EXTRA_EQUAL(event, "local", "local");
        TEST_CHECK_EXTRA_EQUAL(event, "scope", "local");

        // event <- global:
        // {"all":"event","event":"event","global":"global","local":"local","scope":"local"}
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_EXTRA_EQUAL(event, "all", "event");
        TEST_CHECK_EXTRA_EQUAL(event, "event", "event");
        TEST_CHECK_EXTRA_EQUAL(event, "global", "global");
        TEST_CHECK_EXTRA_EQUAL(event, "local", "local");
        TEST_CHECK_EXTRA_EQUAL(event, "scope", "local");

        sentry__scope_free(local_scope);
        sentry_value_decref(event);
    }

#undef TEST_CHECK_EXTRA_EQUAL

    sentry_close();
}

SENTRY_TEST(scope_fingerprint)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    // global:
    // ["global1", "global2"]
    sentry_set_fingerprint("global1", "global2", NULL);

    SENTRY_WITH_SCOPE (global_scope) {
        // event:
        // null
        sentry_value_t event = sentry_value_new_object();

        // event <- global:
        // ["global1", "global2"]
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_JSON_VALUE(sentry_value_get_by_key(event, "fingerprint"),
            "[\"global1\",\"global2\"]");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // event:
        // ["event1", "event2"]
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t fingerprint = sentry_value_new_list();
            sentry_value_append(fingerprint, sentry_value_new_string("event1"));
            sentry_value_append(fingerprint, sentry_value_new_string("event2"));
            sentry_value_set_by_key(event, "fingerprint", fingerprint);
        }

        // event <- global:
        // ["event1", "event2"]
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_JSON_VALUE(sentry_value_get_by_key(event, "fingerprint"),
            "[\"event1\",\"event2\"]");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // local:
        // ["local1", "local2"]
        sentry_scope_t *local_scope = sentry_local_scope_new();
        sentry_scope_set_fingerprint(local_scope, "local1", "local2", NULL);

        // event:
        // ["event1", "event2"]
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t fingerprint = sentry_value_new_list();
            sentry_value_append(fingerprint, sentry_value_new_string("event1"));
            sentry_value_append(fingerprint, sentry_value_new_string("event2"));
            sentry_value_set_by_key(event, "fingerprint", fingerprint);
        }

        // event <- local:
        // ["event1", "event2"]
        sentry__scope_apply_to_event(
            local_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_JSON_VALUE(sentry_value_get_by_key(event, "fingerprint"),
            "[\"event1\",\"event2\"]");

        // event <- global:
        // ["event1", "event2"]
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_JSON_VALUE(sentry_value_get_by_key(event, "fingerprint"),
            "[\"event1\",\"event2\"]");

        sentry__scope_free(local_scope);
        sentry_value_decref(event);
    }

    sentry_close();
}

SENTRY_TEST(scope_fingerprint_n)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    sentry_set_fingerprint_n(
        "alphabet", (size_t)5, "0123456789", (size_t)8, "xyz", (size_t)1, NULL);

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t event = sentry_value_new_object();

        sentry__scope_apply_to_event(scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_JSON_VALUE(sentry_value_get_by_key(event, "fingerprint"),
            "[\"alpha\",\"01234567\",\"x\"]");

        sentry_value_decref(event);
    }

    sentry_close();
}

SENTRY_TEST(scope_tags)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

#define TEST_CHECK_TAG_EQUAL(event, key, value)                                \
    do {                                                                       \
        sentry_value_t tags = sentry_value_get_by_key(event, "tags");          \
        TEST_CHECK_STRING_EQUAL(                                               \
            sentry_value_as_string(sentry_value_get_by_key(tags, key)),        \
            value);                                                            \
    } while (0)

    // global:
    // {"all":"global","scope":"global","global":"global"}
    sentry_set_tag("all", "global");
    sentry_set_tag("global", "global");
    sentry_set_tag("scope", "global");

    SENTRY_WITH_SCOPE (global_scope) {
        // event:
        // {"all":"event","event":"event"}
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t event_tags = sentry_value_new_object();
            sentry_value_set_by_key(
                event_tags, "all", sentry_value_new_string("event"));
            sentry_value_set_by_key(
                event_tags, "event", sentry_value_new_string("event"));
            sentry_value_set_by_key(event, "tags", event_tags);
        }

        // event <- global:
        // {"all":"event","event":"event","global":"global","scope":"global"}
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_TAG_EQUAL(event, "all", "event");
        TEST_CHECK_TAG_EQUAL(event, "event", "event");
        TEST_CHECK_TAG_EQUAL(event, "global", "global");
        TEST_CHECK_TAG_EQUAL(event, "scope", "global");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // local:
        // {"all":"scope","scope":"scope","local":"local"}
        sentry_scope_t *local_scope = sentry_local_scope_new();
        sentry_scope_set_tag(local_scope, "all", "local");
        sentry_scope_set_tag(local_scope, "local", "local");
        sentry_scope_set_tag(local_scope, "scope", "local");

        // event:
        // {"all":"event","event":"event"}
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t event_tags = sentry_value_new_object();
            sentry_value_set_by_key(
                event_tags, "all", sentry_value_new_string("event"));
            sentry_value_set_by_key(
                event_tags, "event", sentry_value_new_string("event"));
            sentry_value_set_by_key(event, "tags", event_tags);
        }

        // event <- local:
        // {"all":"event","event":"event","local":"local","scope":"local"}
        sentry__scope_apply_to_event(
            local_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_TAG_EQUAL(event, "all", "event");
        TEST_CHECK_TAG_EQUAL(event, "event", "event");
        TEST_CHECK_TAG_EQUAL(event, "local", "local");
        TEST_CHECK_TAG_EQUAL(event, "scope", "local");

        // event <- global:
        // {"all":"event","event":"event","global":"global","local":"local","scope":"local"}
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_TAG_EQUAL(event, "all", "event");
        TEST_CHECK_TAG_EQUAL(event, "event", "event");
        TEST_CHECK_TAG_EQUAL(event, "global", "global");
        TEST_CHECK_TAG_EQUAL(event, "local", "local");
        TEST_CHECK_TAG_EQUAL(event, "scope", "local");

        sentry__scope_free(local_scope);
        sentry_value_decref(event);
    }

#undef TEST_CHECK_TAG_EQUAL

    sentry_close();
}

SENTRY_TEST(scope_user)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    // global: {"id":"1","username":"global","email":"@global"}
    sentry_set_user(sentry_value_new_user("1", "global", "@global", NULL));

    SENTRY_WITH_SCOPE (global_scope) {
        // event: {"id":"2","username":"event"}
        sentry_value_t event = sentry_value_new_object();
        sentry_value_set_by_key(
            event, "user", sentry_value_new_user("2", "event", NULL, NULL));

        // event <- global: {"id":"2","username":"event"}
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_JSON_VALUE(sentry_value_get_by_key(event, "user"),
            "{\"id\":\"2\",\"username\":\"event\"}");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // local: {"id":"2","username":"local","email":"@local"}
        sentry_scope_t *local_scope = sentry_local_scope_new();
        sentry_scope_set_user(
            local_scope, sentry_value_new_user("2", "local", "@local", NULL));

        // event: {"id":"3","username":"event"}
        sentry_value_t event = sentry_value_new_object();
        sentry_value_set_by_key(
            event, "user", sentry_value_new_user("3", "event", NULL, NULL));

        // event <- local: {"id":"3","username":"event"}
        sentry__scope_apply_to_event(
            local_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_JSON_VALUE(sentry_value_get_by_key(event, "user"),
            "{\"id\":\"3\",\"username\":\"event\"}");

        // event <- local: {"id":"3","username":"event"}
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_JSON_VALUE(sentry_value_get_by_key(event, "user"),
            "{\"id\":\"3\",\"username\":\"event\"}");

        sentry__scope_free(local_scope);
        sentry_value_decref(event);
    }

    sentry_close();
}

SENTRY_TEST(scope_user_id)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "http://keya@127.0.0.1/42");
    sentry_init(options);

    // null user ID -> installation ID
    TEST_ASSERT(!!options->run->installation_id);
    TEST_CHECK_INT_EQUAL(strlen(options->run->installation_id), 36);
    sentry_set_user(sentry_value_new_user(NULL, "alice", NULL, NULL));
    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t event = sentry_value_new_object();
        sentry__scope_apply_to_event(scope, options, event, SENTRY_SCOPE_NONE);
        sentry_value_t user = sentry_value_get_by_key(event, "user");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(user, "id")),
            options->run->installation_id);
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(user, "username")),
            "alice");
        sentry_value_decref(event);
    }

    // empty user ID
    sentry_set_user(sentry_value_new_user("", "bob", NULL, NULL));
    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t event = sentry_value_new_object();
        sentry__scope_apply_to_event(scope, options, event, SENTRY_SCOPE_NONE);
        sentry_value_t user = sentry_value_get_by_key(event, "user");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(user, "id")), "");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(user, "username")),
            "bob");
        sentry_value_decref(event);
    }

    // non-empty user ID
    sentry_set_user(sentry_value_new_user("42", "carol", NULL, NULL));
    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t event = sentry_value_new_object();
        sentry__scope_apply_to_event(scope, options, event, SENTRY_SCOPE_NONE);
        sentry_value_t user = sentry_value_get_by_key(event, "user");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(user, "id")), "42");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(user, "username")),
            "carol");
        sentry_value_decref(event);
    }

    // empty local scope does not shadow global user ID
    sentry_set_user(sentry_value_new_user("12345", "dave", NULL, NULL));
    sentry_value_t event = sentry_value_new_object();
    sentry_scope_t *local_scope = sentry_local_scope_new();
    sentry__scope_apply_to_event(
        local_scope, options, event, SENTRY_SCOPE_BREADCRUMBS);
    sentry__scope_free(local_scope);
    SENTRY_WITH_SCOPE (scope) {
        sentry__scope_apply_to_event(scope, options, event, SENTRY_SCOPE_ALL);
    }
    sentry_value_t user = sentry_value_get_by_key(event, "user");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(user, "id")), "12345");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(user, "username")),
        "dave");
    sentry_value_decref(event);

    // remove_user -> no user on event (installation ID suppressed)
    sentry_remove_user();
    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t event = sentry_value_new_object();
        sentry__scope_apply_to_event(scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK(
            sentry_value_is_null(sentry_value_get_by_key(event, "user")));
        sentry_value_decref(event);
    }

    sentry_close();
}

SENTRY_TEST(scope_level)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

#define TEST_CHECK_LEVEL_EQUAL(event, value)                                   \
    do {                                                                       \
        sentry_value_t level = sentry_value_get_by_key(event, "level");        \
        TEST_CHECK_STRING_EQUAL(sentry_value_as_string(level), value);         \
    } while (0)

    // global: warning
    sentry_set_level(SENTRY_LEVEL_WARNING);

    SENTRY_WITH_SCOPE (global_scope) {
        // event: null
        sentry_value_t event = sentry_value_new_object();

        // event <- global: warning
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_LEVEL_EQUAL(event, "warning");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // event: info
        sentry_value_t event = sentry_value_new_object();
        sentry_value_set_by_key(
            event, "level", sentry_value_new_string("info"));

        // event <- global: info
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_LEVEL_EQUAL(event, "info");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // local: fatal
        sentry_scope_t *local_scope = sentry_local_scope_new();
        sentry_scope_set_level(local_scope, SENTRY_LEVEL_FATAL);

        // event: debug
        sentry_value_t event = sentry_value_new_object();
        sentry_value_set_by_key(
            event, "level", sentry_value_new_string("debug"));

        // event <- local: debug
        sentry__scope_apply_to_event(
            local_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_LEVEL_EQUAL(event, "debug");

        // event <- global: debug
        sentry__scope_apply_to_event(
            local_scope, options, event, SENTRY_SCOPE_NONE);
        TEST_CHECK_LEVEL_EQUAL(event, "debug");

        sentry__scope_free(local_scope);
        sentry_value_decref(event);
    }

#undef TEST_CHECK_LEVEL_EQUAL

    sentry_close();
}

static sentry_value_t
breadcrumb_ts(const char *message, uint64_t ts)
{
    sentry_value_t breadcrumb = sentry_value_new_breadcrumb(NULL, message);
    sentry_value_set_by_key(breadcrumb, "timestamp",
        sentry__value_new_string_owned(sentry__usec_time_to_iso8601(ts)));
    return breadcrumb;
}

SENTRY_TEST(scope_breadcrumbs)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_max_breadcrumbs(options, 5);
    sentry_init(options);

    // global: ["global1", "global4"]
    sentry_add_breadcrumb(breadcrumb_ts("global1", 1));
    sentry_add_breadcrumb(breadcrumb_ts("global4", 4));

#define TEST_CHECK_MESSAGE_EQUAL(breadcrumbs, index, message)                  \
    TEST_CHECK_STRING_EQUAL(                                                   \
        sentry_value_as_string(sentry_value_get_by_key(                        \
            sentry_value_get_by_index(breadcrumbs, index), "message")),        \
        message)

    SENTRY_WITH_SCOPE (global_scope) {
        // event: null
        sentry_value_t event = sentry_value_new_object();

        // event <- global: ["global1", "global4"]
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_BREADCRUMBS);

        sentry_value_t result = sentry_value_get_by_key(event, "breadcrumbs");
        TEST_CHECK(sentry_value_get_type(result) == SENTRY_VALUE_TYPE_LIST);
        TEST_CHECK_INT_EQUAL(sentry_value_get_length(result), 2);
        TEST_CHECK_MESSAGE_EQUAL(result, 0, "global1");
        TEST_CHECK_MESSAGE_EQUAL(result, 1, "global4");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // event: ["event3", "event5"]
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t breadcrumbs = sentry_value_new_list();
            sentry_value_append(breadcrumbs, breadcrumb_ts("event3", 3));
            sentry_value_append(breadcrumbs, breadcrumb_ts("event5", 5));
            sentry_value_set_by_key(event, "breadcrumbs", breadcrumbs);
        }

        // event <- global: ["global1", "event3", "global4", "event5"]
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_BREADCRUMBS);

        sentry_value_t result = sentry_value_get_by_key(event, "breadcrumbs");
        TEST_CHECK(sentry_value_get_type(result) == SENTRY_VALUE_TYPE_LIST);
        TEST_CHECK_INT_EQUAL(sentry_value_get_length(result), 4);
        TEST_CHECK_MESSAGE_EQUAL(result, 0, "global1");
        TEST_CHECK_MESSAGE_EQUAL(result, 1, "event3");
        TEST_CHECK_MESSAGE_EQUAL(result, 2, "global4");
        TEST_CHECK_MESSAGE_EQUAL(result, 3, "event5");

        sentry_value_decref(event);
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // local: ["local2", "local6"]
        sentry_scope_t *local_scope = sentry_local_scope_new();
        sentry_scope_add_breadcrumb(local_scope, breadcrumb_ts("local2", 2));
        sentry_scope_add_breadcrumb(local_scope, breadcrumb_ts("local6", 6));

        // event: ["event3", "event5"]
        sentry_value_t event = sentry_value_new_object();
        {
            sentry_value_t breadcrumbs = sentry_value_new_list();
            sentry_value_append(breadcrumbs, breadcrumb_ts("event3", 3));
            sentry_value_append(breadcrumbs, breadcrumb_ts("event5", 5));
            sentry_value_set_by_key(event, "breadcrumbs", breadcrumbs);
        }

        // event <- local: ["local2", "event3", "event5", "local6"]
        sentry__scope_apply_to_event(
            local_scope, options, event, SENTRY_SCOPE_BREADCRUMBS);

        sentry_value_t result = sentry_value_get_by_key(event, "breadcrumbs");
        TEST_CHECK(sentry_value_get_type(result) == SENTRY_VALUE_TYPE_LIST);
        TEST_CHECK_INT_EQUAL(sentry_value_get_length(result), 4);
        TEST_CHECK_MESSAGE_EQUAL(result, 0, "local2");
        TEST_CHECK_MESSAGE_EQUAL(result, 1, "event3");
        TEST_CHECK_MESSAGE_EQUAL(result, 2, "event5");
        TEST_CHECK_MESSAGE_EQUAL(result, 3, "local6");

        // event <- global: ["local2", "event3", "global4", "event5", "local6"]
        sentry__scope_apply_to_event(
            global_scope, options, event, SENTRY_SCOPE_BREADCRUMBS);

        result = sentry_value_get_by_key(event, "breadcrumbs");
        TEST_CHECK(sentry_value_get_type(result) == SENTRY_VALUE_TYPE_LIST);
        TEST_CHECK_INT_EQUAL(sentry_value_get_length(result), 5);
        TEST_CHECK_MESSAGE_EQUAL(result, 0, "local2");
        TEST_CHECK_MESSAGE_EQUAL(result, 1, "event3");
        TEST_CHECK_MESSAGE_EQUAL(result, 2, "global4");
        TEST_CHECK_MESSAGE_EQUAL(result, 3, "event5");
        TEST_CHECK_MESSAGE_EQUAL(result, 4, "local6");

        sentry__scope_free(local_scope);
        sentry_value_decref(event);
    }

    sentry_close();
}

static sentry_value_t
before_breadcrumb_discard_cb(sentry_value_t breadcrumb, void *data)
{
    (void)data;
    sentry_value_decref(breadcrumb);
    return sentry_value_new_null();
}

SENTRY_TEST(before_breadcrumb_discard)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_before_breadcrumb(
        options, before_breadcrumb_discard_cb, NULL);
    sentry_init(options);

    sentry_add_breadcrumb(sentry_value_new_breadcrumb(NULL, "discarded"));

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t event = sentry_value_new_object();
        sentry__scope_apply_to_event(
            scope, options, event, SENTRY_SCOPE_BREADCRUMBS);

        sentry_value_t breadcrumbs
            = sentry_value_get_by_key(event, "breadcrumbs");
        TEST_CHECK(sentry_value_is_null(breadcrumbs)
            || sentry_value_get_length(breadcrumbs) == 0);

        sentry_value_decref(event);
    }

    sentry_close();
}

static sentry_value_t
before_breadcrumb_modify_cb(sentry_value_t breadcrumb, void *data)
{
    (void)data;
    sentry_value_set_by_key(
        breadcrumb, "message", sentry_value_new_string("modified"));
    return breadcrumb;
}

SENTRY_TEST(before_breadcrumb_modify)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_before_breadcrumb(
        options, before_breadcrumb_modify_cb, NULL);
    sentry_init(options);

    sentry_add_breadcrumb(sentry_value_new_breadcrumb(NULL, "original"));

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t event = sentry_value_new_object();
        sentry__scope_apply_to_event(
            scope, options, event, SENTRY_SCOPE_BREADCRUMBS);

        sentry_value_t breadcrumbs
            = sentry_value_get_by_key(event, "breadcrumbs");
        TEST_CHECK_INT_EQUAL(sentry_value_get_length(breadcrumbs), 1);
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_index(breadcrumbs, 0), "message")),
            "modified");

        sentry_value_decref(event);
    }

    sentry_close();
}

static sentry_value_t
before_breadcrumb_passthrough_cb(sentry_value_t breadcrumb, void *data)
{
    (void)data;
    return breadcrumb;
}

SENTRY_TEST(before_breadcrumb_passthrough)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_before_breadcrumb(
        options, before_breadcrumb_passthrough_cb, NULL);
    sentry_init(options);

    sentry_add_breadcrumb(sentry_value_new_breadcrumb(NULL, "kept"));

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t event = sentry_value_new_object();
        sentry__scope_apply_to_event(
            scope, options, event, SENTRY_SCOPE_BREADCRUMBS);

        sentry_value_t breadcrumbs
            = sentry_value_get_by_key(event, "breadcrumbs");
        TEST_CHECK_INT_EQUAL(sentry_value_get_length(breadcrumbs), 1);
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_index(breadcrumbs, 0), "message")),
            "kept");

        sentry_value_decref(event);
    }

    sentry_close();
}

SENTRY_TEST(scope_global_attributes)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    // Test setting a valid attribute on the global scope
    sentry_value_t valid_attr = sentry_value_new_attribute(
        sentry_value_new_string("test_value"), NULL);
    sentry_set_attribute("valid_key", valid_attr);

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t attributes = scope->attributes;
        sentry_value_t retrieved_attr
            = sentry_value_get_by_key(attributes, "valid_key");

        // Check that the attribute was set
        TEST_CHECK(!sentry_value_is_null(retrieved_attr));
        TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                    retrieved_attr, "type")),
            "string");
        TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                    retrieved_attr, "value")),
            "test_value");
    }

    // Test that invalid attributes (missing 'value' or 'type') are not set
    sentry_value_t invalid_attr_no_value = sentry_value_new_object();
    sentry_value_set_by_key(
        invalid_attr_no_value, "type", sentry_value_new_string("string"));
    // Missing 'value' field
    sentry_set_attribute("invalid_no_value", invalid_attr_no_value);

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t attributes = scope->attributes;
        sentry_value_t retrieved_attr
            = sentry_value_get_by_key(attributes, "invalid_no_value");

        // Check that the attribute was NOT set
        TEST_CHECK(sentry_value_is_null(retrieved_attr));
    }
    sentry_value_decref(invalid_attr_no_value);

    // Test invalid attribute missing 'type'
    sentry_value_t invalid_attr_no_type = sentry_value_new_object();
    sentry_value_set_by_key(
        invalid_attr_no_type, "value", sentry_value_new_string("some_value"));
    // Missing 'type' field
    sentry_set_attribute("invalid_no_type", invalid_attr_no_type);

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t attributes = scope->attributes;
        sentry_value_t retrieved_attr
            = sentry_value_get_by_key(attributes, "invalid_no_type");

        // Check that the attribute was NOT set
        TEST_CHECK(sentry_value_is_null(retrieved_attr));
    }
    sentry_value_decref(invalid_attr_no_type);

    // Test removing an attribute
    sentry_remove_attribute("valid_key");

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t attributes = scope->attributes;
        sentry_value_t retrieved_attr
            = sentry_value_get_by_key(attributes, "valid_key");

        // Check that the attribute was removed
        TEST_CHECK(sentry_value_is_null(retrieved_attr));
    }

    // Test setting attribute with _n variant
    sentry_value_t attr_n
        = sentry_value_new_attribute(sentry_value_new_int32(42), "percent");
    sentry_set_attribute_n("key_n", 5, attr_n);

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t attributes = scope->attributes;
        sentry_value_t retrieved_attr
            = sentry_value_get_by_key(attributes, "key_n");

        // Check that the attribute was set
        TEST_CHECK(!sentry_value_is_null(retrieved_attr));
        TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                    retrieved_attr, "type")),
            "integer");
        TEST_CHECK(sentry_value_as_int32(
                       sentry_value_get_by_key(retrieved_attr, "value"))
            == 42);
        TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                    retrieved_attr, "unit")),
            "percent");
    }

    sentry_close();
}

SENTRY_TEST(scope_local_attributes)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    // global:
    // {"all":"global","scope":"global","global":"global"}
    sentry_set_attribute("all",
        sentry_value_new_attribute(sentry_value_new_string("global"), NULL));
    sentry_set_attribute("global",
        sentry_value_new_attribute(sentry_value_new_string("global"), NULL));
    sentry_set_attribute("scope",
        sentry_value_new_attribute(sentry_value_new_string("global"), NULL));

    SENTRY_WITH_SCOPE (global_scope) {
        sentry_value_t attributes = global_scope->attributes;

        // Verify global attributes are set
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_key(attributes, "all"), "value")),
            "global");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_key(attributes, "global"), "value")),
            "global");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_key(attributes, "scope"), "value")),
            "global");
    }

    SENTRY_WITH_SCOPE (global_scope) {
        // local:
        // {"all":"local","scope":"local","local":"local"}
        sentry_scope_t *local_scope = sentry_local_scope_new();
        sentry__scope_set_attribute(local_scope, "all",
            sentry_value_new_attribute(sentry_value_new_string("local"), NULL));
        sentry__scope_set_attribute(local_scope, "local",
            sentry_value_new_attribute(sentry_value_new_string("local"), NULL));
        sentry__scope_set_attribute(local_scope, "scope",
            sentry_value_new_attribute(sentry_value_new_string("local"), NULL));

        sentry_value_t local_attributes = local_scope->attributes;

        // Verify local attributes are set
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_key(local_attributes, "all"), "value")),
            "local");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_key(local_attributes, "local"), "value")),
            "local");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_key(local_attributes, "scope"), "value")),
            "local");

        // Verify global scope still has its own attributes
        sentry_value_t global_attributes = global_scope->attributes;
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_key(global_attributes, "all"), "value")),
            "global");
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(
                sentry_value_get_by_key(global_attributes, "global"), "value")),
            "global");

        sentry__scope_free(local_scope);
    }

    // Test removing attributes from global scope
    sentry_remove_attribute("all");

    SENTRY_WITH_SCOPE (scope) {
        sentry_value_t attributes = scope->attributes;
        TEST_CHECK(
            sentry_value_is_null(sentry_value_get_by_key(attributes, "all")));
        // Other attributes should still exist
        TEST_CHECK(!sentry_value_is_null(
            sentry_value_get_by_key(attributes, "global")));
        TEST_CHECK(!sentry_value_is_null(
            sentry_value_get_by_key(attributes, "scope")));
    }

    // Test _n variants with local scope
    SENTRY_WITH_SCOPE (global_scope) {
        sentry_scope_t *local_scope = sentry_local_scope_new();
        sentry__scope_set_attribute_n(local_scope, "test_key", 8,
            sentry_value_new_attribute(sentry_value_new_int32(100), "percent"));

        sentry_value_t local_attributes = local_scope->attributes;
        sentry_value_t attr
            = sentry_value_get_by_key(local_attributes, "test_key");

        TEST_CHECK(!sentry_value_is_null(attr));
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(attr, "type")),
            "integer");
        TEST_CHECK(sentry_value_as_int32(sentry_value_get_by_key(attr, "value"))
            == 100);
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(attr, "unit")),
            "percent");

        // Remove using _n variant
        sentry__scope_remove_attribute_n(local_scope, "test_key", 8);
        TEST_CHECK(sentry_value_is_null(
            sentry_value_get_by_key(local_attributes, "test_key")));

        sentry__scope_free(local_scope);
    }

    // Test that invalid attributes are not set on local scope
    SENTRY_WITH_SCOPE (global_scope) {
        sentry_scope_t *local_scope = sentry_local_scope_new();

        // Try to set invalid attribute (missing 'value')
        sentry_value_t invalid_attr = sentry_value_new_object();
        sentry_value_set_by_key(
            invalid_attr, "type", sentry_value_new_string("string"));
        sentry__scope_set_attribute(local_scope, "invalid", invalid_attr);

        sentry_value_t local_attributes = local_scope->attributes;
        TEST_CHECK(sentry_value_is_null(
            sentry_value_get_by_key(local_attributes, "invalid")));
        sentry_value_decref(invalid_attr);

        sentry__scope_free(local_scope);
    }

    sentry_close();
}
