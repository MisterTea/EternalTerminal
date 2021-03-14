#ifndef SENTRY_UNIX_PAGEALLOCATOR_H_INCLUDED
#define SENTRY_UNIX_PAGEALLOCATOR_H_INCLUDED

#include "sentry_boot.h"

/**
 * Returns the state of the page allocator.
 */
bool sentry__page_allocator_enabled(void);

/**
 * Enables the special page allocator, which is used instead of `malloc` inside
 * of signal handlers.
 * Once it is enabled, it can not be safely disabled without leaking memory.
 */
void sentry__page_allocator_enable(void);

/**
 * This is a replacement for `malloc`, but will return an allocation from
 * anonymously mapped pages.
 */
void *sentry__page_allocator_alloc(size_t size);

#if SENTRY_UNITTEST
/**
 * This disables the page allocator, which invalidates every allocation that was
 * done through it. Therefore it is only safe to use in unit tests
 */
void sentry__page_allocator_disable(void);
#endif

#endif
