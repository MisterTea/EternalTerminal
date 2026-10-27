#ifndef SENTRY_VALUE_H_INCLUDED
#define SENTRY_VALUE_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_json.h"
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
 * Creates a new String Value from the `uuid` that conforms to
 * the structure of a span ID.
 * See also `sentry__span_uuid_as_string`.
 */
sentry_value_t sentry__value_new_span_uuid(const sentry_uuid_t *uuid);

/**
 * Creates a new String Value from the `uuid` in a form meant for
 * ingestion as an internal ID.
 * See also `sentry_internal_uuid_as_string`.
 */
sentry_value_t sentry__value_new_internal_uuid(const sentry_uuid_t *uuid);

/**
 * Creates a new String Value from the `uuid`.
 * See also `sentry_uuid_as_string`.
 */
sentry_value_t sentry__value_new_uuid(const sentry_uuid_t *uuid);

/**
 * Creates a new Event with the given `event_id`.
 * Used by Crashpad to allow associating feedback with the crash event.
 */
sentry_value_t sentry__value_new_event_with_id(const sentry_uuid_t *event_id);

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
 * Iterates over the key/value pairs of an object value. The callback receives a
 * borrowed reference for each value. Does nothing if `value` is not an object.
 */
void sentry__value_foreach_key_value(sentry_value_t value,
    void (*callback)(const char *key, sentry_value_t value, void *userdata),
    void *userdata);

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
 * Deep-merges object src into dst.
 *
 * For each key-value pair in the src object the same key in the dst object
 * will be set to the value from src.  If both the dst value and the src value
 * are objects themselves they are stepped into recursively instead of
 * overriding the entire dst object.
 *
 * If src is null nothing needs to be merged and this is handled gracefully,
 * otherwise if dst is any other type than an object or src is neither an
 * object nor null an error is returned.
 *
 * Returns 0 on success.
 */
int sentry__value_merge_objects(sentry_value_t dst, sentry_value_t src);

/**
 * Writes the given `value` into the `jsonwriter`.
 */
void sentry__jsonwriter_write_value(
    sentry_jsonwriter_t *jw, sentry_value_t value);

/**
 * Serializes `value` into JSON and optionally returns the byte length.
 */
char *sentry__value_to_json(sentry_value_t value, size_t *len_out);

/**
 * Adds a typed attribute to the attributes object.
 * No-op if the attribute already exists (preserves user precedence).
 * Takes ownership of `value`.
 */
void sentry__value_add_attribute(sentry_value_t attributes,
    sentry_value_t value, const char *type, const char *name);

/**
 * Deserialize a sentry value from msgpack.
 *
 * If the buffer contains multiple sequential msgpack values (as in flat buffers
 * like breadcrumb files), they are automatically wrapped in a list.
 *
 * The returned value must be released with `sentry_value_decref`.
 */
sentry_value_t sentry__value_from_msgpack(const char *buf, size_t buf_len);

/**
 * Merges two breadcrumb lists in timestamp order, keeping at most `max` items.
 * Returns a new list with the merged breadcrumbs.
 */
sentry_value_t sentry__value_merge_breadcrumbs(
    sentry_value_t list_a, sentry_value_t list_b, size_t max);

#endif
