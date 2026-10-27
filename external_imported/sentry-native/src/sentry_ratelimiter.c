#include "sentry_ratelimiter.h"
#include "sentry_alloc.h"
#include "sentry_slice.h"
#include "sentry_utils.h"

#define MAX_RATE_LIMITS 5
#define MAX_RETRY_AFTER (24 * 60 * 60) // 24h

struct sentry_rate_limiter_s {
    uint64_t disabled_until[MAX_RATE_LIMITS];
};

static uint64_t
calculate_disabled_until(uint64_t retry_after)
{
    if (retry_after > MAX_RETRY_AFTER) {
        retry_after = MAX_RETRY_AFTER;
    }
    return sentry__monotonic_time() + retry_after * 1000; // ms
}

sentry_rate_limiter_t *
sentry__rate_limiter_new(void)
{
    sentry_rate_limiter_t *rl = SENTRY_MAKE(sentry_rate_limiter_t);
    if (rl) {
        rl->disabled_until[SENTRY_RL_CATEGORY_ANY] = 0;
        rl->disabled_until[SENTRY_RL_CATEGORY_ERROR] = 0;
        rl->disabled_until[SENTRY_RL_CATEGORY_SESSION] = 0;
        rl->disabled_until[SENTRY_RL_CATEGORY_TRANSACTION] = 0;
        rl->disabled_until[SENTRY_RL_CATEGORY_REPLAY] = 0;
    }
    return rl;
}

bool
sentry__rate_limiter_update_from_header(
    sentry_rate_limiter_t *rl, const char *sentry_header)
{
    sentry_slice_t slice = sentry__slice_from_str(sentry_header);

    while (true) {
        slice = sentry__slice_trim(slice);
        uint64_t retry_after = 0;
        if (!sentry__slice_consume_uint64(&slice, &retry_after)) {
            return false;
        }
        retry_after = calculate_disabled_until(retry_after);

        if (!sentry__slice_consume_if(&slice, ':')) {
            return false;
        }

        sentry_slice_t categories = sentry__slice_split_at(slice, ':');
        if (categories.len == 0) {
            rl->disabled_until[SENTRY_RL_CATEGORY_ANY] = retry_after;
        }

        while (categories.len > 0) {
            sentry_slice_t category = sentry__slice_split_at(categories, ';');
            if (sentry__slice_eqs(category, "error")) {
                rl->disabled_until[SENTRY_RL_CATEGORY_ERROR] = retry_after;
            } else if (sentry__slice_eqs(category, "session")) {
                rl->disabled_until[SENTRY_RL_CATEGORY_SESSION] = retry_after;
            } else if (sentry__slice_eqs(category, "transaction")) {
                rl->disabled_until[SENTRY_RL_CATEGORY_TRANSACTION]
                    = retry_after;
            } else if (sentry__slice_eqs(category, "replay")) {
                rl->disabled_until[SENTRY_RL_CATEGORY_REPLAY] = retry_after;
            }

            categories = sentry__slice_advance(categories, category.len);
            sentry__slice_consume_if(&categories, ';');
        }

        size_t next = sentry__slice_find(slice, ',');
        if (next != (size_t)-1) {
            slice = sentry__slice_advance(slice, next + 1);
        } else {
            break;
        }
    }

    return true;
}

bool
sentry__rate_limiter_update_from_http_retry_after(
    sentry_rate_limiter_t *rl, const char *retry_after)
{
    sentry_slice_t slice = sentry__slice_from_str(retry_after);
    uint64_t eta = 60;
    sentry__slice_consume_uint64(&slice, &eta);
    rl->disabled_until[SENTRY_RL_CATEGORY_ANY] = calculate_disabled_until(eta);
    return true;
}

bool
sentry__rate_limiter_update_from_429(sentry_rate_limiter_t *rl)
{
    rl->disabled_until[SENTRY_RL_CATEGORY_ANY]
        = sentry__monotonic_time() + 60 * 1000;
    return true;
}

bool
sentry__rate_limiter_is_disabled(const sentry_rate_limiter_t *rl, int category)
{
    uint64_t now = sentry__monotonic_time();
    return rl->disabled_until[SENTRY_RL_CATEGORY_ANY] > now
        || rl->disabled_until[category] > now;
}

void
sentry__rate_limiter_free(sentry_rate_limiter_t *rl)
{
    if (!rl) {
        return;
    }
    sentry_free(rl);
}

#ifdef SENTRY_UNITTEST
uint64_t
sentry__rate_limiter_get_disabled_until(
    const sentry_rate_limiter_t *rl, int category)
{
    return rl->disabled_until[category];
}
#endif
