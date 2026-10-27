#ifndef SENTRY_UUID_H_INCLUDED
#define SENTRY_UUID_H_INCLUDED

#include "sentry_boot.h"

/**
 * Converts a sentry UUID to a string representation used for internal
 * sentry UUIDs such as event IDs.
 */
void sentry__internal_uuid_as_string(const sentry_uuid_t *uuid, char str[37]);

/**
 * Converts a sentry UUID to a string representation used for span IDs.
 */
void sentry__span_uuid_as_string(const sentry_uuid_t *uuid, char str[17]);

/**
 * Converts a sentry UUID to a string representation used for a filename with
 * the given suffix.
 */
char *sentry__uuid_as_filename(const sentry_uuid_t *uuid, const char *suffix);

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Create a new UUID from the windows-native GUID type.
 */
sentry_uuid_t sentry__uuid_from_native(const GUID *guid);
#endif

#endif
