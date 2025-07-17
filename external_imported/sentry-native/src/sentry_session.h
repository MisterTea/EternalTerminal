#ifndef SENTRY_SESSION_H_INCLUDED
#define SENTRY_SESSION_H_INCLUDED

#include "sentry_boot.h"

#include "sentry_path.h"
#include "sentry_utils.h"

struct sentry_jsonwriter_s;

/**
 * This represents a session, with the number of errors, a status and other
 * metadata.
 */
typedef struct sentry_session_s {
    char *release;
    char *environment;
    sentry_uuid_t session_id;
    sentry_value_t distinct_id;
    uint64_t started_us;
    uint64_t duration_us;
    uint64_t errors;
    sentry_session_status_t status;
    long init;
} sentry_session_t;

/**
 * This creates a new session.
 */
sentry_session_t *sentry__session_new(void);

/**
 * This will free a previously allocated session.
 */
void sentry__session_free(sentry_session_t *session);

/**
 * This will write the gives session into the json writer `jw`.
 */
void sentry__session_to_json(
    const sentry_session_t *session, struct sentry_jsonwriter_s *jw);

/**
 * Given a JSON string, this will parse and create a session out of it, or NULL
 * on failure.
 */
sentry_session_t *sentry__session_from_json(const char *buf, size_t buf_len);

/**
 * This will read the JSON serialized session from `path`, or return NULL on
 * failure.
 */
sentry_session_t *sentry__session_from_path(const sentry_path_t *path);

/**
 * This will end the current session with an explicit `status` code.
 */
sentry_session_t *sentry__end_current_session_with_status(
    sentry_session_status_t status);

/**
 * This will add `error_count` new errors to the current session.
 */
void sentry__record_errors_on_current_session(uint32_t error_count);

/**
 * This will update a sessions `distinct_id`, which is based on the user.
 */
void sentry__session_sync_user(sentry_session_t *session, sentry_value_t user);

#endif
