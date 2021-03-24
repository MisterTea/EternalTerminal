#ifndef SENTRY_RANDOM_H_INCLUDED
#define SENTRY_RANDOM_H_INCLUDED

#include "sentry_boot.h"

/**
 * Utility function to get random bytes.
 */
int sentry__getrandom(void *dst, size_t len);

#endif
