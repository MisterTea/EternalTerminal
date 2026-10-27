#ifndef SENTRY_APP_HANG_LATCH_H_INCLUDED
#define SENTRY_APP_HANG_LATCH_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_value.h"

#define SENTRY_APP_HANG_MAX_FRAMES 128

bool sentry__app_hang_should_capture(
    uint64_t hb, uint64_t now, uint64_t timeout_ms, uint64_t last_fired_hb);

typedef struct {
    uint64_t target_tid;
    uint64_t last_heartbeat_ms;
} sentry_app_hang_latch_t;

uint64_t sentry__app_hang_current_tid(void);
sentry_app_hang_latch_t sentry__app_hang_current_latch(void);
void sentry__app_hang_latch_reset(void);

// Enables/disables the heartbeat fast-path. The watchdog monitor sets this on
// start and clears it on stop, so sentry_app_hang_heartbeat() is a cheap no-op
// when detection is not running.
void sentry__app_hang_set_active(bool active);

// Whether app-hang detection is currently armed.
bool sentry__app_hang_is_active(void);

sentry_value_t sentry__app_hang_make_event(
    void **ips, size_t frame_count, uint64_t freeze_ms);

#endif
