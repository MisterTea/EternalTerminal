#include "sentry_testsupport.h"
#include <stdlib.h>
#include <string.h>

#ifdef SENTRY_EMBED_INFO
// The embedded info symbol is always compiled into the test executable itself,
// so we always use extern for the declaration
#    ifdef _WIN32
extern SENTRY_API const char sentry_library_info[];
#    else
extern const char sentry_library_info[];
#    endif
#endif

SENTRY_TEST(embedded_info_basic)
{
#ifdef SENTRY_EMBED_INFO
    // Test that the embedded info string exists and has expected format
    TEST_ASSERT(sentry_library_info != NULL);
    TEST_CHECK(strlen(sentry_library_info) > 0);

    // Test that required fields are present
    TEST_CHECK(strstr(sentry_library_info, "SENTRY_VERSION:") != NULL);
    TEST_CHECK(strstr(sentry_library_info, "PLATFORM:") != NULL);
    TEST_CHECK(strstr(sentry_library_info, "BUILD:") != NULL);
    TEST_CHECK(strstr(sentry_library_info, "CONFIG:") != NULL);
    TEST_CHECK(strstr(sentry_library_info, "END") != NULL);
#else
    SKIP_TEST();
#endif
}

SENTRY_TEST(embedded_info_format)
{
#ifdef SENTRY_EMBED_INFO
    // Test that the string is properly semicolon-separated
#    ifdef _WIN32
    char *info = _strdup(sentry_library_info);
#    else
    char *info = strdup(sentry_library_info);
#    endif
    TEST_ASSERT(info != NULL);

    int field_count = 0;
    char *token = strtok(info, ";");
    while (token != NULL) {
        // Each field should contain a colon (except END)
        if (strcmp(token, "END") != 0) {
            TEST_CHECK(strchr(token, ':') != NULL);
        }
        field_count++;
        token = strtok(NULL, ";");
    }

    // Should have at least 5 fields: VERSION, PLATFORM, BUILD, VARIANT, CONFIG,
    // END
    TEST_CHECK(field_count >= 6);

    free(info);
#else
    SKIP_TEST();
#endif
}

SENTRY_TEST(embedded_info_sentry_version)
{
#ifdef SENTRY_EMBED_INFO
    // Test that SENTRY_VERSION field contains the actual SDK version
    const char *version_field = strstr(sentry_library_info, "SENTRY_VERSION:");
    TEST_ASSERT(version_field != NULL);

    // Extract the version value
    const char *version_start = version_field + strlen("SENTRY_VERSION:");
    const char *version_end = strchr(version_start, ';');
    TEST_ASSERT(version_end != NULL);

    size_t version_len = version_end - version_start;
    TEST_ASSERT(version_len > 0);

    // Use dynamic allocation or larger buffer
    char *embedded_version = malloc(version_len + 1);
    TEST_ASSERT(embedded_version != NULL);
    strncpy(embedded_version, version_start, version_len);
    embedded_version[version_len] = '\0';

    // Version should contain at least one dot (e.g., "0.10.0")
    TEST_CHECK(strchr(embedded_version, '.') != NULL);

    // Test that embedded version is the base version (major.minor.patch)
    // It should NOT contain build metadata (e.g., +build-id)
    TEST_CHECK(strchr(embedded_version, '+') == NULL);

    // Verify embedded version starts with the same prefix as SENTRY_SDK_VERSION
    // (handles cases where SENTRY_SDK_VERSION may include build metadata)
    size_t embedded_len = strlen(embedded_version);
    TEST_CHECK(
        strncmp(embedded_version, SENTRY_SDK_VERSION, embedded_len) == 0);

    free(embedded_version);
#else
    SKIP_TEST();
#endif
}

SENTRY_TEST(embedded_info_build_id)
{
#ifdef SENTRY_EMBED_INFO
    // Test that BUILD field is present and non-empty
    const char *build_field = strstr(sentry_library_info, "BUILD:");
    TEST_ASSERT(build_field != NULL);

    // Extract the build ID value
    const char *build_start = build_field + strlen("BUILD:");
    const char *build_end = strchr(build_start, ';');
    TEST_ASSERT(build_end != NULL);

    size_t build_len = build_end - build_start;
    TEST_ASSERT(build_len > 0);

    // Build ID should not be empty
    char *build_id = malloc(build_len + 1);
    TEST_ASSERT(build_id != NULL);
    strncpy(build_id, build_start, build_len);
    build_id[build_len] = '\0';

    TEST_CHECK(strlen(build_id) > 0);

    free(build_id);
#else
    SKIP_TEST();
#endif
}

SENTRY_TEST(embedded_info_disabled)
{
#ifndef SENTRY_EMBED_INFO
    // When SENTRY_EMBED_INFO is not defined, the feature is properly disabled
    TEST_CHECK(1); // Always pass - confirms the feature is disabled
#else
    SKIP_TEST(); // Skip when embedding is enabled
#endif
}
