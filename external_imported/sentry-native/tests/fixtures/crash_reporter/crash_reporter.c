#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    define NOMINMAX
#    define _CRT_SECURE_NO_WARNINGS
#endif

#include "sentry.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(_MSC_VER)
#    include <windows.h>
int
wmain(int argc, wchar_t *argv[])
{
    // Set console output to UTF-8 so `fwprintf` outputs the wides correctly
    SetConsoleOutputCP(CP_UTF8);

    if (argc != 2) {
        fwprintf(stderr, L"Usage: %ls <envelope>\n", argv[0]);
        return EXIT_FAILURE;
    }

    wchar_t *path = argv[1];
    sentry_envelope_t *envelope = sentry_envelope_read_from_filew(path);
    if (!envelope) {
        fwprintf(stderr, L"ERROR: %ls (%ls)\n", path, _wcserror(errno));
        return EXIT_FAILURE;
    }
#else
int
main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <envelope>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *path = argv[1];
    sentry_envelope_t *envelope = sentry_envelope_read_from_file(path);
    if (!envelope) {
        fprintf(stderr, "ERROR: %s (%s)\n", path, strerror(errno));
        return EXIT_FAILURE;
    }
#endif

    sentry_value_t dsn = sentry_envelope_get_header(envelope, "dsn");
    sentry_value_t event_id = sentry_envelope_get_header(envelope, "event_id");

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_backend(options, NULL);
    sentry_options_set_dsn(options, sentry_value_as_string(dsn));
    sentry_options_set_debug(options, true);
    sentry_init(options);

    sentry_uuid_t uuid
        = sentry_uuid_from_string(sentry_value_as_string(event_id));
    sentry_value_t feedback = sentry_value_new_feedback(
        "some-message", "some-email", "some-name", &uuid);

    sentry_capture_envelope(envelope);
    sentry_capture_feedback(feedback);

    sentry_close();

#if defined(_WIN32) && defined(_MSC_VER)
    _wremove(path);
#else
    remove(path);
#endif
}
