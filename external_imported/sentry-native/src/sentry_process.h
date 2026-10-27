#ifndef SENTRY_PROCESS_H_INCLUDED
#define SENTRY_PROCESS_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_path.h"

/**
 * Spawns a new detached subprocess with the given executable and variable
 * arguments as UTF-8 narrow strings terminated by a NULL.
 *
 * Note: the arguments are not sanitized.
 */
void sentry__process_spawn(
    const sentry_path_t *executable, const char *arg0, ...);

#endif
