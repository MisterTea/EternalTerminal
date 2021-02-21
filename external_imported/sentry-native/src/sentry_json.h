#ifndef SENTRY_JSON_H_INCLUDED
#define SENTRY_JSON_H_INCLUDED

#include "sentry_boot.h"

struct sentry_jsonwriter_s;
typedef struct sentry_jsonwriter_s sentry_jsonwriter_t;

/**
 * This creates a new in-memory JSON writer, based on `sentry_stringbuilder_s`.
 */
sentry_jsonwriter_t *sentry__jsonwriter_new_in_memory(void);

/**
 * Deallocates a JSON writer.
 */
void sentry__jsonwriter_free(sentry_jsonwriter_t *jw);

/**
 * This will consume and deallocate the JSON writer, returning the generated
 * JSON string, and writing its length into `len_out`.
 */
char *sentry__jsonwriter_into_string(sentry_jsonwriter_t *jw, size_t *len_out);

/**
 * Write a `null` into the JSON.
 */
void sentry__jsonwriter_write_null(sentry_jsonwriter_t *jw);

/**
 * Write a JSON boolean.
 */
void sentry__jsonwriter_write_bool(sentry_jsonwriter_t *jw, bool val);

/**
 * Write a 32-bit number, which will be encoded in a JSON number.
 */
void sentry__jsonwriter_write_int32(sentry_jsonwriter_t *jw, int32_t val);

/**
 * Write a 64-bit float, encoded as JSON number.
 */
void sentry__jsonwriter_write_double(sentry_jsonwriter_t *jw, double val);

/**
 * Write a zero-terminated string.
 */
void sentry__jsonwriter_write_str(sentry_jsonwriter_t *jw, const char *val);

/**
 * Write a UUID as a JSON string.
 * See `sentry_uuid_as_string`.
 */
void sentry__jsonwriter_write_uuid(
    sentry_jsonwriter_t *jw, const sentry_uuid_t *uuid);

/**
 * This will write a millisecond resolution timestamp formattad as an ISO8601
 * string.
 * See `sentry__msec_time_to_iso8601`.
 */
void sentry__jsonwriter_write_msec_timestamp(
    sentry_jsonwriter_t *jw, uint64_t time);

/**
 * Writes the *Key* part of an object Key-Value pair.
 */
void sentry__jsonwriter_write_key(sentry_jsonwriter_t *jw, const char *val);

/**
 * Start a new JSON array.
 * Needs to be closed with `sentry__jsonwriter_write_list_end`.
 */
void sentry__jsonwriter_write_list_start(sentry_jsonwriter_t *jw);

/**
 * End the previously started JSON array.
 */
void sentry__jsonwriter_write_list_end(sentry_jsonwriter_t *jw);

/**
 * Start a new JSON object.
 * Needs to be closed with `sentry__jsonwriter_write_object_end`.
 */
void sentry__jsonwriter_write_object_start(sentry_jsonwriter_t *jw);

/**
 * End the previously started JSON object.
 */
void sentry__jsonwriter_write_object_end(sentry_jsonwriter_t *jw);

/**
 * Parse the given JSON string into a new Value.
 */
sentry_value_t sentry__value_from_json(const char *buf, size_t buflen);

#endif
