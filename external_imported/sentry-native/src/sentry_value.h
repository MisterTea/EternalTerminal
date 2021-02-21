#ifndef SENTRY_VALUE_H_INCLUDED
#define SENTRY_VALUE_H_INCLUDED

#include "sentry_boot.h"

/**
 * Create a new Value from an owned string.
 */
sentry_value_t sentry__value_new_string_owned(char *s);

#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Create a new Value from a Wide String.
 */
sentry_value_t sentry__value_new_string_from_wstr(const wchar_t *s);
#endif

/**
 * Create a new String Value, with the hex-formatted value of `addr`.
 */
sentry_value_t sentry__value_new_addr(uint64_t addr);

/**
 * Creates a new String Value, with a hex representation of `bytes`.
 */
sentry_value_t sentry__value_new_hexstring(const uint8_t *bytes, size_t len);

/**
 * Creates a new String Value from the `uuid`.
 * See also `sentry_uuid_as_string`.
 */
sentry_value_t sentry__value_new_uuid(const sentry_uuid_t *uuid);

/**
 * Creates a new String Value from the given `level`.
 * This can be `debug`, `warning`, `error`, `fatal`, or `info`.
 */
sentry_value_t sentry__value_new_level(sentry_level_t level);

/**
 * Creates a new List Value with a capacity of `size`.
 */
sentry_value_t sentry__value_new_list_with_size(size_t size);

/**
 * Creates a new Object Value with a capacity of `size`.
 */
sentry_value_t sentry__value_new_object_with_size(size_t size);

/**
 * This will parse the Value into a UUID, or return a `nil` UUID on error.
 * See also `sentry_uuid_from_string`.
 */
sentry_uuid_t sentry__value_as_uuid(sentry_value_t value);

/**
 * This will create a simplified string representation of the value.
 * Lists, Objects and `null` will be converted to an empty string.
 */
char *sentry__value_stringify(sentry_value_t value);

/**
 * Performs a shallow clone.
 * On a frozen value this produces an unfrozen one.
 */
sentry_value_t sentry__value_clone(sentry_value_t value);

/**
 * This appends `v` to the List `value`.
 * It will remove the first value of the list, is case the total number if items
 * would exceed `max`.
 *
 * Returns 0 on success.
 */
int sentry__value_append_bounded(
    sentry_value_t value, sentry_value_t v, size_t max);

/**
 * Parse the given JSON string into a new Value.
 */
sentry_value_t sentry__value_from_json(const char *buf, size_t buflen);

#endif
