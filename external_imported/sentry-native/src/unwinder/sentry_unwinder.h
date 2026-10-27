#ifndef SENTRY_UNWINDER_H_INCLUDED
#define SENTRY_UNWINDER_H_INCLUDED

#include "sentry_boot.h"

#include <stdint.h>

typedef struct {
    uintptr_t lo, hi;
} mem_range_t;

#if defined(SENTRY_PLATFORM_LINUX)

#    include <stddef.h>
#    include <sys/types.h>

#    define SENTRY_REMOTE_UNWIND_MAX_REGISTER_NAME 16
#    define SENTRY_REMOTE_UNWIND_MAX_REGISTERS 64
#    define SENTRY_REMOTE_UNWIND_MAX_SYMBOL 256

typedef struct {
    char name[SENTRY_REMOTE_UNWIND_MAX_REGISTER_NAME];
    uint64_t value;
} sentry_remote_register_t;

typedef struct {
    uint64_t ip;
    char symbol[SENTRY_REMOTE_UNWIND_MAX_SYMBOL];
    uint64_t symbol_offset;
} sentry_remote_frame_t;

typedef struct {
    size_t count;
    sentry_remote_register_t values[SENTRY_REMOTE_UNWIND_MAX_REGISTERS];
} sentry_remote_registers_t;

size_t sentry__unwind_stack_from_thread(pid_t tid,
    sentry_remote_frame_t *frames, size_t max_frames,
    sentry_remote_registers_t *registers);

#endif

#endif // SENTRY_UNWINDER_H_INCLUDED
