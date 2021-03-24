#ifndef SENTRY_WINDOWS_DBGHELP_H_INCLUDED
#define SENTRY_WINDOWS_DBGHELP_H_INCLUDED

#include "sentry_boot.h"

/**
 * This will initialize the symbol handler for the current process, and return a
 * `HANDLE` to it.
 */
HANDLE sentry__init_dbghelp(void);

#endif
