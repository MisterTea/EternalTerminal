#ifndef SENTRY_SYMBOLIZER_H_INCLUDED
#define SENTRY_SYMBOLIZER_H_INCLUDED

#include "sentry_boot.h"

typedef struct sentry_frame_info_s {
    void *load_addr;
    void *symbol_addr;
    void *instruction_addr;
    const char *symbol;
    const char *object_name;
} sentry_frame_info_t;

/**
 * This will symbolize the provided `addr`, and call `func` with a populated
 * frame info and the given `data`.
 */
bool sentry__symbolize(
    void *addr, void (*func)(const sentry_frame_info_t *, void *), void *data);

#endif
