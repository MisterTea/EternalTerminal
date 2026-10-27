#include "sentry_client_report.h"
#include "sentry_sync.h"

// Counters for discarded events, indexed by [reason][category]
static volatile long g_discard_counts[SENTRY_DISCARD_REASON_MAX]
                                     [SENTRY_DATA_CATEGORY_MAX]
    = { { 0 } };

void
sentry__client_report_discard(sentry_discard_reason_t reason,
    sentry_data_category_t category, long quantity)
{
    if (reason >= SENTRY_DISCARD_REASON_MAX
        || category >= SENTRY_DATA_CATEGORY_MAX || quantity <= 0) {
        return;
    }

    sentry__atomic_fetch_and_add(
        (long *)&g_discard_counts[reason][category], quantity);
}

bool
sentry__client_report_has_pending(void)
{
    for (int r = 0; r < SENTRY_DISCARD_REASON_MAX; r++) {
        for (int c = 0; c < SENTRY_DATA_CATEGORY_MAX; c++) {
            if (sentry__atomic_fetch((long *)&g_discard_counts[r][c]) > 0) {
                return true;
            }
        }
    }
    return false;
}

bool
sentry__client_report_save(sentry_client_report_t *report)
{
    bool has_counts = false;
    for (int r = 0; r < SENTRY_DISCARD_REASON_MAX; r++) {
        for (int c = 0; c < SENTRY_DATA_CATEGORY_MAX; c++) {
            report->counts[r][c]
                = sentry__atomic_store((long *)&g_discard_counts[r][c], 0);
            if (report->counts[r][c] > 0) {
                has_counts = true;
            }
        }
    }
    return has_counts;
}

void
sentry__client_report_restore(sentry_client_report_t *report)
{
    if (!report) {
        return;
    }
    for (int r = 0; r < SENTRY_DISCARD_REASON_MAX; r++) {
        for (int c = 0; c < SENTRY_DATA_CATEGORY_MAX; c++) {
            if (report->counts[r][c] > 0) {
                sentry__atomic_fetch_and_add(
                    (long *)&g_discard_counts[r][c], report->counts[r][c]);
            }
        }
    }
    memset(report, 0, sizeof(*report));
}

void
sentry__client_report_reset(void)
{
    for (int r = 0; r < SENTRY_DISCARD_REASON_MAX; r++) {
        for (int c = 0; c < SENTRY_DATA_CATEGORY_MAX; c++) {
            sentry__atomic_store((long *)&g_discard_counts[r][c], 0);
        }
    }
}
