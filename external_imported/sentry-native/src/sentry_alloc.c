#include "sentry_alloc.h"
#include "sentry_sync.h"
#include <stdlib.h>
#include <string.h>

/* on unix platforms we add support for a simplistic page allocator that can
   be enabled to make code async safe */
#if defined(SENTRY_PLATFORM_UNIX)
#    include "sentry_unix_pageallocator.h"
#    define WITH_PAGE_ALLOCATOR
#endif

void *
sentry_malloc(size_t size)
{
#ifdef WITH_PAGE_ALLOCATOR
    if (sentry__page_allocator_enabled()) {
        return sentry__page_allocator_alloc(size);
    }
#endif
    return malloc(size);
}

void *
sentry__calloc(size_t count, size_t size)
{
    if (count && size > SIZE_MAX / count) {
        return NULL;
    }
#ifdef WITH_PAGE_ALLOCATOR
    if (sentry__page_allocator_enabled()) {
        // the page allocator is a bump allocator backed by mmap(MAP_ANONYMOUS),
        // which the OS guarantees to be zeroed on first use, and the page
        // allocator never reuses freed allocations
        return sentry__page_allocator_alloc(count * size);
    }
#endif
    return calloc(count, size);
}

void
sentry_free(void *ptr)
{
#ifdef WITH_PAGE_ALLOCATOR
    /* page allocator can't free */
    if (sentry__page_allocator_enabled()) {
        return;
    }
#endif
    free(ptr);
}
