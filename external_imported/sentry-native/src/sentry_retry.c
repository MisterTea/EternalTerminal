#include "sentry_retry.h"
#include "sentry_alloc.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_logger.h"
#include "sentry_options.h"
#include "sentry_utils.h"

#include <stdlib.h>
#include <string.h>

#define SENTRY_RETRY_ATTEMPTS 6
#define SENTRY_RETRY_INTERVAL (15 * 60 * 1000)
#define SENTRY_RETRY_THROTTLE 100

typedef enum {
    SENTRY_RETRY_STARTUP = 0,
    SENTRY_RETRY_RUNNING = 1,
    SENTRY_RETRY_SEALED = 2
} sentry_retry_state_t;

typedef enum {
    SENTRY_POLL_IDLE = 0,
    SENTRY_POLL_SCHEDULED = 1,
    SENTRY_POLL_SHUTDOWN = 2
} sentry_poll_state_t;

struct sentry_retry_s {
    sentry_run_t *run;
    sentry_cache_keep_t cache_keep;
    uint64_t startup_time;
    volatile long state;
    volatile long scheduled;
    sentry_bgworker_t *bgworker;
    sentry_retry_send_func_t send_cb;
    void *send_data;
    sentry_mutex_t sealed_lock;
};

sentry_retry_t *
sentry__retry_new(const sentry_options_t *options)
{
    sentry_retry_t *retry = SENTRY_MAKE(sentry_retry_t);
    if (!retry) {
        return NULL;
    }
    sentry__mutex_init(&retry->sealed_lock);
    retry->run = sentry__run_incref(options->run);
    retry->cache_keep = options->cache_keep;
    retry->startup_time = sentry__usec_time() / 1000;
    return retry;
}

void
sentry__retry_free(sentry_retry_t *retry)
{
    if (!retry) {
        return;
    }
    sentry__mutex_free(&retry->sealed_lock);
    sentry__run_free(retry->run);
    sentry_free(retry);
}

uint64_t
sentry__retry_backoff(int count)
{
    // TODO: consider adding jitter and shortening the poll interval to spread
    // out retries when multiple envelopes (esp. large attachments) pile up.
    return (uint64_t)SENTRY_RETRY_INTERVAL << MIN(MAX(count, 0), 5);
}

typedef struct {
    sentry_path_t *path;
    uint64_t ts;
    int count;
    char uuid[37];
} retry_item_t;

static int
compare_retry_items(const void *a, const void *b)
{
    const retry_item_t *ia = a;
    const retry_item_t *ib = b;
    if (ia->ts != ib->ts) {
        return ia->ts < ib->ts ? -1 : 1;
    }
    if (ia->count != ib->count) {
        return ia->count - ib->count;
    }
    return strcmp(ia->uuid, ib->uuid);
}

static bool
handle_result(sentry_retry_t *retry, const retry_item_t *item,
    const sentry_envelope_t *envelope, int status_code)
{
    // Only network failures (status_code < 0) trigger retries. HTTP responses
    // including 5xx (500, 502, 503, 504) are discarded:
    // https://develop.sentry.dev/sdk/foundations/transport/offline-caching/#dealing-with-network-failures

    // network failure with retries remaining: bump count & re-enqueue
    if (item->count + 1 < SENTRY_RETRY_ATTEMPTS && status_code < 0) {
        sentry_path_t *new_path = sentry__run_make_cache_path(retry->run,
            sentry__usec_time() / 1000, item->count + 1, item->uuid);
        if (new_path) {
            if (sentry__path_rename(item->path, new_path) != 0) {
                SENTRY_WARNF(
                    "failed to rename retry envelope \"%s\"", item->path->path);
            }
            sentry__path_free(new_path);
        }
        return true;
    }

    bool exhausted = item->count + 1 >= SENTRY_RETRY_ATTEMPTS;

    // network failure with retries exhausted
    if (exhausted && status_code < 0) {
        if (retry->cache_keep) {
            SENTRY_WARNF("max retries (%d) reached, moving envelope to cache",
                SENTRY_RETRY_ATTEMPTS);
        } else {
            SENTRY_WARNF("max retries (%d) reached, discarding envelope",
                SENTRY_RETRY_ATTEMPTS);
        }
    }

    // cache on last attempt
    if (exhausted && retry->cache_keep && status_code < 0) {
        if (retry->cache_keep == SENTRY_CACHE_KEEP_ALWAYS) {
            sentry_path_t *cached_path
                = sentry__run_make_cache_path(retry->run, 0, -1, item->uuid);
            bool cached = cached_path && sentry__path_is_file(cached_path);
            sentry__path_free(cached_path);
            if (cached) {
                sentry__path_remove(item->path);
                return false;
            }
        }
        if (!sentry__run_move_cache(retry->run, item->path, -1)) {
            sentry__cache_remove_envelope(item->path);
        }
        return false;
    }

    if (retry->cache_keep == SENTRY_CACHE_KEEP_ALWAYS
        && sentry__run_write_cache(retry->run, envelope, -1)) {
        sentry__path_remove(item->path);
    } else {
        sentry__cache_remove_envelope(item->path);
    }
    return false;
}

size_t
sentry__retry_send(sentry_retry_t *retry, uint64_t before,
    sentry_retry_send_func_t send_cb, void *data)
{
    if (sentry__run_should_skip_upload(retry->run)) {
        return 1; // keep the poll alive until consent is given
    }

    sentry_pathiter_t *piter
        = sentry__path_iter_directory(retry->run->cache_path);
    if (!piter) {
        return 0;
    }

    size_t item_cap = 16;
    retry_item_t *items = sentry_malloc(item_cap * sizeof(retry_item_t));
    if (!items) {
        sentry__pathiter_free(piter);
        return 0;
    }

    size_t total = 0;
    size_t eligible = 0;
    uint64_t now = before > 0 ? 0 : sentry__usec_time() / 1000;

    const sentry_path_t *p;
    while ((p = sentry__pathiter_next(piter)) != NULL) {
        const char *fname = sentry__path_filename(p);
        uint64_t ts;
        int count;
        const char *uuid;
        if (!sentry__parse_cache_filename(fname, &ts, &count, &uuid)
            || count < 0) {
            continue;
        }
        if (before > 0 && ts >= before) {
            continue;
        }
        total++;
        if (!before
            && (now < ts || (now - ts) < sentry__retry_backoff(count))) {
            continue;
        }
        if (eligible == item_cap) {
            item_cap *= 2;
            retry_item_t *tmp = sentry_malloc(item_cap * sizeof(retry_item_t));
            if (!tmp) {
                break;
            }
            memcpy(tmp, items, eligible * sizeof(retry_item_t));
            sentry_free(items);
            items = tmp;
        }
        retry_item_t *item = &items[eligible];
        item->path = sentry__path_clone(p);
        if (!item->path) {
            break;
        }
        item->ts = ts;
        item->count = count;
        memcpy(item->uuid, uuid, 36);
        item->uuid[36] = '\0';
        eligible++;
    }
    sentry__pathiter_free(piter);

    if (eligible > 1) {
        qsort(items, eligible, sizeof(retry_item_t), compare_retry_items);
    }

    for (size_t i = 0; i < eligible; i++) {
        sentry_envelope_t *envelope = sentry__envelope_from_path(items[i].path);
        if (!envelope) {
            sentry__cache_remove_envelope(items[i].path);
            total--;
        } else {
            SENTRY_DEBUGF("retrying envelope (%d/%d)", items[i].count + 1,
                SENTRY_RETRY_ATTEMPTS);
            int status_code = send_cb(envelope, data);
            bool keep_retry
                = handle_result(retry, &items[i], envelope, status_code);
            sentry_envelope_free(envelope);
            if (!keep_retry) {
                total--;
            }
            // stop on network failure to avoid wasting time on a dead
            // connection; remaining envelopes stay untouched for later
            if (status_code < 0) {
                break;
            }
        }
    }

    for (size_t i = 0; i < eligible; i++) {
        sentry__path_free(items[i].path);
    }
    sentry_free(items);
    return total;
}

static void
retry_poll_task(void *_retry, void *_state)
{
    (void)_state;
    sentry_retry_t *retry = _retry;
    uint64_t before
        = sentry__atomic_fetch(&retry->state) == SENTRY_RETRY_STARTUP
        ? retry->startup_time
        : 0;
    // CAS instead of unconditional store to preserve SENTRY_POLL_SHUTDOWN
    sentry__atomic_compare_swap(
        &retry->scheduled, SENTRY_POLL_SCHEDULED, SENTRY_POLL_IDLE);
    if (sentry__retry_send(retry, before, retry->send_cb, retry->send_data)
        && sentry__atomic_compare_swap(
            &retry->scheduled, SENTRY_POLL_IDLE, SENTRY_POLL_SCHEDULED)) {
        sentry__bgworker_submit_delayed(retry->bgworker, retry_poll_task, NULL,
            retry, SENTRY_RETRY_INTERVAL);
    }
    // subsequent polls use backoff instead of the startup time filter
    sentry__atomic_compare_swap(
        &retry->state, SENTRY_RETRY_STARTUP, SENTRY_RETRY_RUNNING);
}

void
sentry__retry_start(sentry_retry_t *retry, sentry_bgworker_t *bgworker,
    sentry_retry_send_func_t send_cb, void *send_data)
{
    retry->bgworker = bgworker;
    retry->send_cb = send_cb;
    retry->send_data = send_data;
    sentry__atomic_store(&retry->scheduled, SENTRY_POLL_SCHEDULED);
    sentry__bgworker_submit_delayed(
        bgworker, retry_poll_task, NULL, retry, SENTRY_RETRY_THROTTLE);
}

static void
retry_flush_task(void *_retry, void *_state)
{
    (void)_state;
    sentry_retry_t *retry = _retry;
    sentry__retry_send(
        retry, retry->startup_time, retry->send_cb, retry->send_data);
}

static bool
drop_task_cb(void *_data, void *_ctx)
{
    (void)_data;
    (void)_ctx;
    return true;
}

void
sentry__retry_shutdown(sentry_retry_t *retry)
{
    if (retry) {
        // drop the delayed poll and prevent retry_poll_task from re-arming
        sentry__bgworker_foreach_matching(
            retry->bgworker, retry_poll_task, drop_task_cb, NULL);
        sentry__atomic_store(&retry->scheduled, SENTRY_POLL_SHUTDOWN);
        sentry__bgworker_submit(retry->bgworker, retry_flush_task, NULL, retry);
    }
}

void
sentry__retry_seal(sentry_retry_t *retry)
{
    if (retry) {
        // prevent duplicate writes from a still-running detached worker
        sentry__mutex_lock(&retry->sealed_lock);
        sentry__atomic_store(&retry->state, SENTRY_RETRY_SEALED);
        sentry__mutex_unlock(&retry->sealed_lock);
    }
}

static void
retry_trigger_task(void *_retry, void *_state)
{
    (void)_state;
    sentry_retry_t *retry = _retry;
    sentry__retry_send(retry, UINT64_MAX, retry->send_cb, retry->send_data);
}

void
sentry__retry_trigger(sentry_retry_t *retry)
{
    sentry__bgworker_submit(retry->bgworker, retry_trigger_task, NULL, retry);
}

bool
sentry__retry_enqueue(sentry_retry_t *retry, const sentry_envelope_t *envelope)
{
    sentry__mutex_lock(&retry->sealed_lock);
    if (sentry__atomic_fetch(&retry->state) == SENTRY_RETRY_SEALED) {
        sentry__mutex_unlock(&retry->sealed_lock);
        return false;
    }
    if (!sentry__run_write_cache(retry->run, envelope, 0)) {
        sentry__mutex_unlock(&retry->sealed_lock);
        return false;
    }
    sentry__mutex_unlock(&retry->sealed_lock);

    sentry__atomic_compare_swap(
        &retry->state, SENTRY_RETRY_STARTUP, SENTRY_RETRY_RUNNING);
    if (sentry__atomic_compare_swap(
            &retry->scheduled, SENTRY_POLL_IDLE, SENTRY_POLL_SCHEDULED)) {
        sentry__bgworker_submit_delayed(retry->bgworker, retry_poll_task, NULL,
            retry, SENTRY_RETRY_INTERVAL);
    }
    return true;
}
