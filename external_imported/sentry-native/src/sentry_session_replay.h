#ifndef SENTRY_SESSION_REPLAY_H_INCLUDED
#define SENTRY_SESSION_REPLAY_H_INCLUDED

#include "sentry_boot.h"

#include "sentry_options.h"
#include "sentry_path.h"

/**
 * Captures a short retroactive video replay and saves it to the specified path.
 *
 * @param path The path where the replay should be saved (typically MP4).
 * @param duration_ms The requested duration in milliseconds.
 * @param pid The process ID whose output should be captured (0 = current
 * process).
 *
 * Returns true if the replay was successfully captured and saved.
 */
bool sentry__session_replay_capture(
    const sentry_path_t *path, uint32_t duration_ms, uint32_t pid);

/**
 * Returns the path where a session replay should be saved.
 */
sentry_path_t *sentry__session_replay_get_path(const sentry_options_t *options);

/**
 * Returns whether the embedder's `<database>/replays/` staging directory holds
 * a replay clip, so callers can skip crash-event work when nothing is staged.
 */
bool sentry__session_replay_has_pending(const sentry_options_t *options);

/**
 * Build and send the session-replay envelope for the crash described by
 * `scope_source`. The replay is identified by the crash event's
 * `contexts.replay.replay_id` and staged by the embedder as
 * `<database>/replays/replay-<id>.{json,mp4}`.
 *
 * Runs only on a crash. The native crash daemon calls it out-of-process with
 * the live transport, delivering the replay same-session. The crashpad,
 * breakpad and inproc backends call it from their crash handlers with a
 * run-dir disk transport, so the replay envelope is persisted next to the
 * crash envelope and delivered on the next launch. The sidecar and mp4 are
 * consumed once and removed after the flush attempt, regardless of whether
 * the envelope was built or sent, so a failed flush is not retried.
 *
 * `scope_source` is the crash event (`<run>/__sentry-event`); its scope fields
 * and trace id are copied onto the replay, and its timestamp ends the replay
 * window. If it is NULL or carries no `contexts.replay.replay_id`, nothing is
 * flushed.
 */
void sentry__session_replay_flush_pending(const sentry_options_t *options,
    sentry_transport_t *transport, sentry_value_t scope_source);

#endif
