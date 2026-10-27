#ifndef SENTRY_LOGS_H_INCLUDED
#define SENTRY_LOGS_H_INCLUDED

#include "sentry_boot.h"

log_return_value_t sentry__logs_log(
    sentry_level_t level, const char *message, va_list args);

/**
 * Sets up the logs timer/flush thread
 */
void sentry__logs_startup(const sentry_options_t *options);

/**
 * Shuts down the logs timer/flush thread.
 */
void sentry__logs_shutdown(uint64_t timeout);

/**
 * Crash-safe logs flush that avoids thread synchronization.
 * This should be used during crash handling to flush logs without
 * waiting for the batching thread to shut down cleanly.
 */
void sentry__logs_flush_crash_safe(void);

/**
 * Begin non-blocking force flush of logs. Returns an opaque token that must
 * be passed to sentry__logs_force_flush_wait to complete the flush.
 */
uintptr_t sentry__logs_force_flush_begin(void);

/**
 * Wait for the logs force flush to complete. Takes the token returned by
 * sentry__logs_force_flush_begin.
 */
void sentry__logs_force_flush_wait(uintptr_t token);

#ifdef SENTRY_UNITTEST
int populate_message_parameters(
    sentry_value_t attributes, const char *message, va_list args);

/**
 * Wait for the logs batching thread to be ready.
 * This is a test-only helper to avoid race conditions in tests.
 */
void sentry__logs_wait_for_thread_startup(void);
#endif

#endif
