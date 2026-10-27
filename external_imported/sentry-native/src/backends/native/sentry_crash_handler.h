#ifndef SENTRY_CRASH_HANDLER_H_INCLUDED
#define SENTRY_CRASH_HANDLER_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_crash_ipc.h"

/**
 * Initialize crash handler (install signal handlers)
 */
int sentry__crash_handler_init(sentry_crash_ipc_t *ipc);

/**
 * Shutdown crash handler (restore previous handlers)
 */
void sentry__crash_handler_shutdown(void);

#endif
