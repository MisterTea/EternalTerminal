#ifndef SENTRY_MINIDUMP_WRITER_H_INCLUDED
#define SENTRY_MINIDUMP_WRITER_H_INCLUDED

#include "../sentry_crash_context.h"
#include "sentry_boot.h"

/**
 * Write a minidump file from crash context.
 *
 * @param ctx Crash context captured from signal/exception handler
 * @param output_path Path where minidump will be written
 * @return 0 on success, -1 on failure
 */
int sentry__write_minidump(
    const sentry_crash_context_t *ctx, const char *output_path);

#endif
