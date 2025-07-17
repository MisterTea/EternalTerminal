#include "sentry.h"
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
