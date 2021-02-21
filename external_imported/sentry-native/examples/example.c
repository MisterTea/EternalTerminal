#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    define _CRT_SECURE_NO_WARNINGS
#endif

#include "sentry.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SENTRY_PLATFORM_WINDOWS
#    include <synchapi.h>
#    define sleep_s(SECONDS) Sleep((SECONDS)*1000)
#else
#    include <unistd.h>
#    define sleep_s(SECONDS) sleep(SECONDS)
#endif

static void
print_envelope(sentry_envelope_t *envelope, void *unused_state)
{
    (void)unused_state;
    size_t size_out = 0;
    char *s = sentry_envelope_serialize(envelope, &size_out);
    printf("%s", s);
    sentry_free(s);
    sentry_envelope_free(envelope);
}

static bool
has_arg(int argc, char **argv, const char *arg)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], arg) == 0) {
            return true;
        }
    }
    return false;
}

static void *invalid_mem = (void *)1;

static void
trigger_crash()
{
    memset((char *)invalid_mem, 1, 100);
}

int
main(int argc, char **argv)
{
    sentry_options_t *options = sentry_options_new();

    // this is an example. for real usage, make sure to set this explicitly to
    // an app specific cache location.
    sentry_options_set_database_path(options, ".sentry-native");

    sentry_options_set_auto_session_tracking(options, false);
    sentry_options_set_symbolize_stacktraces(options, true);

    sentry_options_set_environment(options, "development");
    // sentry defaults this to the `SENTRY_RELEASE` env variable
    if (!has_arg(argc, argv, "release-env")) {
        sentry_options_set_release(options, "test-example-release");
    }

    if (has_arg(argc, argv, "log")) {
        sentry_options_set_debug(options, 1);
    }

    if (has_arg(argc, argv, "attachment")) {
        // assuming the example / test is run directly from the cmake build
        // directory
        sentry_options_add_attachment(options, "./CMakeCache.txt");
    }

    if (has_arg(argc, argv, "stdout")) {
        sentry_options_set_transport(
            options, sentry_transport_new(print_envelope));
    }

    sentry_init(options);

    if (!has_arg(argc, argv, "no-setup")) {
        sentry_set_transaction("test-transaction");
        sentry_set_level(SENTRY_LEVEL_WARNING);
        sentry_set_extra("extra stuff", sentry_value_new_string("some value"));
        sentry_set_extra("…unicode key…",
            // https://xkcd.com/1813/ :-)
            sentry_value_new_string("őá…–🤮🚀¿ 한글 테스트"));
        sentry_set_tag("expected-tag", "some value");
        sentry_set_tag("not-expected-tag", "some value");
        sentry_remove_tag("not-expected-tag");

        sentry_value_t context = sentry_value_new_object();
        sentry_value_set_by_key(
            context, "type", sentry_value_new_string("runtime"));
        sentry_value_set_by_key(
            context, "name", sentry_value_new_string("testing-runtime"));
        sentry_set_context("runtime", context);

        sentry_value_t user = sentry_value_new_object();
        sentry_value_set_by_key(user, "id", sentry_value_new_int32(42));
        sentry_value_set_by_key(
            user, "username", sentry_value_new_string("some_name"));
        sentry_set_user(user);

        sentry_value_t default_crumb
            = sentry_value_new_breadcrumb(NULL, "default level is info");
        sentry_add_breadcrumb(default_crumb);

        sentry_value_t debug_crumb
            = sentry_value_new_breadcrumb("http", "debug crumb");
        sentry_value_set_by_key(
            debug_crumb, "category", sentry_value_new_string("example!"));
        sentry_value_set_by_key(
            debug_crumb, "level", sentry_value_new_string("debug"));
        sentry_add_breadcrumb(debug_crumb);

        sentry_value_t nl_crumb
            = sentry_value_new_breadcrumb(NULL, "lf\ncrlf\r\nlf\n...");
        sentry_value_set_by_key(
            nl_crumb, "category", sentry_value_new_string("something else"));
        sentry_add_breadcrumb(nl_crumb);
    }

    if (has_arg(argc, argv, "start-session")) {
        sentry_start_session();
    }

    if (has_arg(argc, argv, "overflow-breadcrumbs")) {
        for (size_t i = 0; i < 101; i++) {
            char buffer[4];
            snprintf(buffer, 4, "%zu", i);
            sentry_add_breadcrumb(sentry_value_new_breadcrumb(0, buffer));
        }
    }

    if (has_arg(argc, argv, "capture-multiple")) {
        for (size_t i = 0; i < 10; i++) {
            char buffer[10];
            snprintf(buffer, 10, "Event #%zu", i);

            sentry_value_t event = sentry_value_new_message_event(
                SENTRY_LEVEL_INFO, NULL, buffer);
            sentry_capture_event(event);
        }
    }

    if (has_arg(argc, argv, "sleep")) {
        sleep_s(10);
    }

    if (has_arg(argc, argv, "crash")) {
        trigger_crash();
    }

    if (has_arg(argc, argv, "capture-event")) {
        sentry_value_t event = sentry_value_new_message_event(
            SENTRY_LEVEL_INFO, "my-logger", "Hello World!");
        if (has_arg(argc, argv, "add-stacktrace")) {
            sentry_event_value_add_stacktrace(event, NULL, 0);
        }
        sentry_capture_event(event);
    }
    if (has_arg(argc, argv, "capture-exception")) {
        // TODO: Create a convenience API to create a new exception object,
        // and to attach a stacktrace to the exception.
        // See also https://github.com/getsentry/sentry-native/issues/235
        sentry_value_t event = sentry_value_new_event();
        sentry_value_t exception = sentry_value_new_object();
        // for example:
        sentry_value_set_by_key(
            exception, "type", sentry_value_new_string("ParseIntError"));
        sentry_value_set_by_key(exception, "value",
            sentry_value_new_string("invalid digit found in string"));
        sentry_value_t exceptions = sentry_value_new_list();
        sentry_value_append(exceptions, exception);
        sentry_value_t values = sentry_value_new_object();
        sentry_value_set_by_key(values, "values", exceptions);
        sentry_value_set_by_key(event, "exception", values);

        sentry_capture_event(event);
    }

    // make sure everything flushes
    sentry_shutdown();
    if (has_arg(argc, argv, "sleep-after-shutdown")) {
        sleep_s(1);
    }

    if (has_arg(argc, argv, "crash-after-shutdown")) {
        trigger_crash();
    }
}
