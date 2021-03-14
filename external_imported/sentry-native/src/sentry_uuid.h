#ifndef SENTRY_UUID_H_INCLUDED
#define SENTRY_UUID_H_INCLUDED

#include "sentry_boot.h"

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Create a new UUID from the windows-native GUID type.
 */
sentry_uuid_t sentry__uuid_from_native(GUID *guid);
#endif

#endif
