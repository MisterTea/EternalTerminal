#ifndef SENTRY_RETRY_H_INCLUDED
#define SENTRY_RETRY_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_path.h"
#include "sentry_sync.h"

typedef struct sentry_retry_s sentry_retry_t;

typedef int (*sentry_retry_send_func_t)(
    sentry_envelope_t *envelope, void *data);

sentry_retry_t *sentry__retry_new(const sentry_options_t *options);
void sentry__retry_free(sentry_retry_t *retry);

/**
 * Schedules retry polling on `bgworker` using `send_cb`.
 */
void sentry__retry_start(sentry_retry_t *retry, sentry_bgworker_t *bgworker,
    sentry_retry_send_func_t send_cb, void *send_data);

/**
 * Prepares retry for shutdown: drops pending polls and submits a flush task.
 */
void sentry__retry_shutdown(sentry_retry_t *retry);

/**
 * Seals the retry system against further enqueue calls.
 */
void sentry__retry_seal(sentry_retry_t *retry);

/**
 * Writes a failed envelope to the retry dir and schedules a delayed poll.
 */
bool sentry__retry_enqueue(
    sentry_retry_t *retry, const sentry_envelope_t *envelope);

/**
 * Sends eligible retry files via `send_cb`. `before > 0`: send files with
 * ts < before (startup). `before == 0`: use backoff. Returns remaining file
 * count for controlling polling.
 */
size_t sentry__retry_send(sentry_retry_t *retry, uint64_t before,
    sentry_retry_send_func_t send_cb, void *data);

/**
 * Exponential backoff: 15m, 30m, 1h, 2h, 4h, 8h, 8h, ... (capped at 8h).
 */
uint64_t sentry__retry_backoff(int count);

/**
 * Submits a delayed retry poll task on the background worker.
 */
void sentry__retry_trigger(sentry_retry_t *retry);

#endif
