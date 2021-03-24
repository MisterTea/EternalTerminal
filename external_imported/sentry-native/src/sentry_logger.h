#ifndef SENTRY_LOGGER_H_INCLUDED
#define SENTRY_LOGGER_H_INCLUDED

#include "sentry_boot.h"

typedef struct {
    sentry_logger_function_t logger_func;
    void *logger_data;
} sentry_logger_t;

void sentry__logger_set_global(sentry_logger_t logger);

void sentry__logger_defaultlogger(
    sentry_level_t level, const char *message, va_list args, void *data);

const char *sentry__logger_describe(sentry_level_t level);

void sentry__logger_log(sentry_level_t level, const char *message, ...);

#define SENTRY_TRACEF(message, ...)                                            \
    sentry__logger_log(SENTRY_LEVEL_DEBUG, message, __VA_ARGS__)

#define SENTRY_TRACE(message) sentry__logger_log(SENTRY_LEVEL_DEBUG, message)

#define SENTRY_DEBUGF(message, ...)                                            \
    sentry__logger_log(SENTRY_LEVEL_INFO, message, __VA_ARGS__)

#define SENTRY_DEBUG(message) sentry__logger_log(SENTRY_LEVEL_INFO, message)

#define SENTRY_WARNF(message, ...)                                             \
    sentry__logger_log(SENTRY_LEVEL_WARNING, message, __VA_ARGS__)

#define SENTRY_WARN(message) sentry__logger_log(SENTRY_LEVEL_WARNING, message)

#endif
