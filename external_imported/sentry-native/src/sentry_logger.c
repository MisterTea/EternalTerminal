#include "sentry_logger.h"
#include "sentry_core.h"
#include "sentry_options.h"

#include <stdio.h>
#include <string.h>

static sentry_logger_t g_logger = { NULL, NULL };

void
sentry__logger_set_global(sentry_logger_t logger)
{
    g_logger = logger;
}

#if defined(SENTRY_PLATFORM_ANDROID)

#    include <android/log.h>
void
sentry__logger_defaultlogger(
    sentry_level_t level, const char *message, va_list args, void *UNUSED(data))
{
    android_LogPriority priority = ANDROID_LOG_UNKNOWN;
    switch (level) {
    case SENTRY_LEVEL_DEBUG:
        priority = ANDROID_LOG_DEBUG;
        break;
    case SENTRY_LEVEL_INFO:
        priority = ANDROID_LOG_INFO;
        break;
    case SENTRY_LEVEL_WARNING:
        priority = ANDROID_LOG_WARN;
        break;
    case SENTRY_LEVEL_ERROR:
        priority = ANDROID_LOG_ERROR;
        break;
    case SENTRY_LEVEL_FATAL:
        priority = ANDROID_LOG_FATAL;
        break;
    default:
        break;
    }
    __android_log_vprint(priority, "sentry-native", message, args);
}

#else

void
sentry__logger_defaultlogger(
    sentry_level_t level, const char *message, va_list args, void *UNUSED(data))
{
    const char *prefix = "[sentry] ";
    const char *priority = sentry__logger_describe(level);

    size_t len = strlen(prefix) + strlen(priority) + strlen(message) + 2;
    char *format = sentry_malloc(len);
    snprintf(format, len, "%s%s%s\n", prefix, priority, message);

    vfprintf(stderr, format, args);

    sentry_free(format);
}

#endif

const char *
sentry__logger_describe(sentry_level_t level)
{
    switch (level) {
    case SENTRY_LEVEL_DEBUG:
        return "DEBUG ";
    case SENTRY_LEVEL_INFO:
        return "INFO ";
    case SENTRY_LEVEL_WARNING:
        return "WARN ";
    case SENTRY_LEVEL_ERROR:
        return "ERROR ";
    case SENTRY_LEVEL_FATAL:
        return "FATAL ";
    default:
        return "UNKNOWN ";
    }
}

void
sentry__logger_log(sentry_level_t level, const char *message, ...)
{
    sentry_logger_t logger = g_logger;
    if (logger.logger_func) {
        va_list args;
        va_start(args, message);
        logger.logger_func(level, message, args, logger.logger_data);
        va_end(args);
    }
}
