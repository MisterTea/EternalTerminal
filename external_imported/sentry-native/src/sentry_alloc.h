#ifndef SENTRY_ALLOC_H_INCLUDED
#define SENTRY_ALLOC_H_INCLUDED

#include "sentry_boot.h"

void *sentry__calloc(size_t count, size_t size);

/**
 * This is a shortcut for a typed `calloc` that zero-initializes the allocation.
 */
#define SENTRY_MAKE(Type) (Type *)sentry__calloc(1, sizeof(Type))

#endif
