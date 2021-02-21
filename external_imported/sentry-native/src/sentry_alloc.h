#ifndef SENTRY_ALLOC_H_INCLUDED
#define SENTRY_ALLOC_H_INCLUDED

#include "sentry_boot.h"

/**
 * This is a shortcut for a typed `malloc`.
 */
#define SENTRY_MAKE(Type) (Type *)sentry_malloc(sizeof(Type))

#endif
