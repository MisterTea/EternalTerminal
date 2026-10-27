#ifndef SENTRY_LOGGER_H_INCLUDED
#define SENTRY_LOGGER_H_INCLUDED

#include "sentry_boot.h"

typedef struct {
    sentry_logger_function_t logger_func;
    void *logger_data;
    sentry_level_t logger_level;
} sentry_logger_t;

void sentry__logger_set_global(sentry_logger_t logger);

void sentry__logger_defaultlogger(
    sentry_level_t level, const char *message, va_list args, void *data);

const char *sentry__logger_describe(sentry_level_t level);

void sentry__logger_log(sentry_level_t level, const char *message, ...);

void sentry__logger_enable(void);
void sentry__logger_disable(void);

#define SENTRY_TRACEF(message, ...)                                            \
    sentry__logger_log(SENTRY_LEVEL_TRACE, message, __VA_ARGS__)

#define SENTRY_TRACE(message) sentry__logger_log(SENTRY_LEVEL_TRACE, message)

#define SENTRY_DEBUGF(message, ...)                                            \
    sentry__logger_log(SENTRY_LEVEL_DEBUG, message, __VA_ARGS__)

#define SENTRY_DEBUG(message) sentry__logger_log(SENTRY_LEVEL_DEBUG, message)

#define SENTRY_INFOF(message, ...)                                             \
    sentry__logger_log(SENTRY_LEVEL_INFO, message, __VA_ARGS__)

#define SENTRY_INFO(message) sentry__logger_log(SENTRY_LEVEL_INFO, message)

#define SENTRY_WARNF(message, ...)                                             \
    sentry__logger_log(SENTRY_LEVEL_WARNING, message, __VA_ARGS__)

#define SENTRY_WARN(message) sentry__logger_log(SENTRY_LEVEL_WARNING, message)

#define SENTRY_ERRORF(message, ...)                                            \
    sentry__logger_log(SENTRY_LEVEL_ERROR, message, __VA_ARGS__)

#define SENTRY_ERROR(message) sentry__logger_log(SENTRY_LEVEL_ERROR, message)

#define SENTRY_FATALF(message, ...)                                            \
    sentry__logger_log(SENTRY_LEVEL_FATAL, message, __VA_ARGS__)

#define SENTRY_FATAL(message) sentry__logger_log(SENTRY_LEVEL_FATAL, message)

/**
 * Signal/async-safe logging macro for use in signal handlers or other
 * contexts where stdio and malloc are unsafe. Only supports static strings.
 */
#ifdef SENTRY_PLATFORM_UNIX
#    include <unistd.h>
#    define SENTRY_SIGNAL_SAFE_LOG(msg)                                        \
        do {                                                                   \
            static const char _msg[] = "[sentry] " msg "\n";                   \
            (void)!write(STDERR_FILENO, _msg, sizeof(_msg) - 1);               \
        } while (0)
#elif defined(SENTRY_PLATFORM_WINDOWS)
#    define SENTRY_SIGNAL_SAFE_LOG(msg)                                        \
        do {                                                                   \
            static const char _msg[] = "[sentry] " msg "\n";                   \
            OutputDebugStringA(_msg);                                          \
            HANDLE _stderr = GetStdHandle(STD_ERROR_HANDLE);                   \
            if (_stderr && _stderr != INVALID_HANDLE_VALUE) {                  \
                DWORD _written;                                                \
                WriteFile(_stderr, _msg, (DWORD)(sizeof(_msg) - 1), &_written, \
                    NULL);                                                     \
            }                                                                  \
        } while (0)
#else
#    define SENTRY_SIGNAL_SAFE_LOG(msg) ((void)0)
#endif

#endif
