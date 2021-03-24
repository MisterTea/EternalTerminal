#include "sentry_unix_pageallocator.h"
#include "sentry_core.h"
#include "sentry_unix_spinlock.h"

#include <sys/mman.h>
#include <unistd.h>

// some macos versions do not have this yet
#if !defined(MAP_ANONYMOUS) && defined(SENTRY_PLATFORM_DARWIN)
#    define MAP_ANONYMOUS MAP_ANON
#endif

#define ALIGN 8

struct page_header;
struct page_header {
    struct page_header *next;
    size_t num_pages;
};

struct page_allocator_s {
    size_t page_size;
    struct page_header *last_page;
    char *current_page;
    size_t page_offset;
    size_t pages_allocated;
};

static struct page_allocator_s g_page_allocator_backing = { 0 };
static struct page_allocator_s *g_alloc = NULL;
static sentry_spinlock_t g_lock = SENTRY__SPINLOCK_INIT;

bool
sentry__page_allocator_enabled(void)
{
    return !!g_alloc;
}

void
sentry__page_allocator_enable(void)
{
    sentry__spinlock_lock(&g_lock);
    if (!g_alloc) {
        g_alloc = &g_page_allocator_backing;
        g_alloc->page_size = getpagesize();
        g_alloc->last_page = NULL;
        g_alloc->current_page = NULL;
        g_alloc->page_offset = 0;
        g_alloc->pages_allocated = 0;
    }
    sentry__spinlock_unlock(&g_lock);
}

static void *
get_pages(size_t num_pages)
{
    void *rv = mmap(NULL, g_alloc->page_size * num_pages,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rv == MAP_FAILED) {
        return NULL;
    }

#if defined(__has_feature)
#    if __has_feature(memory_sanitizer)
    __msan_unpoison(rv, g_alloc->page_size * num_pages);
#    endif
#endif

    struct page_header *header = (struct page_header *)rv;
    header->next = g_alloc->last_page;
    header->num_pages = num_pages;
    g_alloc->last_page = header;

    g_alloc->pages_allocated += num_pages;

    return rv;
}

void *
sentry__page_allocator_alloc(size_t size)
{
    if (!size) {
        return NULL;
    }

    // make sure the requested size is correctly aligned
    size_t diff = size % ALIGN;
    size = size + ALIGN - diff;

    char *rv = NULL;

    sentry__spinlock_lock(&g_lock);

    // current page is large enough
    if (g_alloc->current_page
        && g_alloc->page_size - g_alloc->page_offset >= size) {
        rv = g_alloc->current_page + g_alloc->page_offset;
        g_alloc->page_offset += size;
        if (g_alloc->page_offset == g_alloc->page_size) {
            g_alloc->page_offset = 0;
            g_alloc->current_page = NULL;
        }
    } else {
        // allocate new pages

        size_t requested_size = size + sizeof(struct page_header);
        size_t pages
            = (requested_size + g_alloc->page_size - 1) / g_alloc->page_size;
        size_t actual_size = g_alloc->page_size * pages;

        rv = get_pages(pages);

        if (rv) {
            size_t diff = actual_size - requested_size;
            g_alloc->page_offset
                = (g_alloc->page_size - diff) % g_alloc->page_size;
            g_alloc->current_page = g_alloc->page_offset
                ? rv + g_alloc->page_size * (pages - 1)
                : NULL;
            rv += sizeof(struct page_header);
        }
    }

    sentry__spinlock_unlock(&g_lock);
    return rv;
}

#if SENTRY_UNITTEST
void
sentry__page_allocator_disable(void)
{
    if (!g_alloc) {
        return;
    }
    struct page_header *next;
    for (struct page_header *cur = g_alloc->last_page; cur; cur = next) {
        next = cur->next;
        munmap(cur, cur->num_pages * g_alloc->page_size);
    }
    g_alloc = NULL;
}
#endif
