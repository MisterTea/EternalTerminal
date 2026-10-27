#include "sentry_logs.h"
#include "sentry_batcher.h"
#include "sentry_core.h"
#include "sentry_envelope.h"
#include "sentry_options.h"
#include "sentry_os.h"
#include "sentry_scope.h"
#include "sentry_value.h"
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static sentry_batcher_ref_t g_batcher = SENTRY_BATCHER_REF_INIT;

typedef enum {
    PRINTF_LENGTH_NONE,
    PRINTF_LENGTH_CHAR,
    PRINTF_LENGTH_SHORT,
    PRINTF_LENGTH_LONG,
    PRINTF_LENGTH_LONG_LONG,
    PRINTF_LENGTH_INTMAX,
    PRINTF_LENGTH_SIZE,
    PRINTF_LENGTH_PTRDIFF,
    PRINTF_LENGTH_LONG_DOUBLE,
} printf_length_t;

#if SIZE_MAX == UINT_MAX
typedef int printf_ssize_t;
#elif SIZE_MAX == ULONG_MAX
typedef long printf_ssize_t;
#elif SIZE_MAX == ULLONG_MAX
typedef long long printf_ssize_t;
#else
typedef ptrdiff_t printf_ssize_t;
#endif

#if PTRDIFF_MAX == INT_MAX
typedef unsigned int printf_uptrdiff_t;
#elif PTRDIFF_MAX == LONG_MAX
typedef unsigned long printf_uptrdiff_t;
#elif PTRDIFF_MAX == LLONG_MAX
typedef unsigned long long printf_uptrdiff_t;
#else
typedef size_t printf_uptrdiff_t;
#endif

static const char *
level_as_string(sentry_level_t level)
{
    switch (level) {
    case SENTRY_LEVEL_TRACE:
        return "trace";
    case SENTRY_LEVEL_DEBUG:
        return "debug";
    case SENTRY_LEVEL_INFO:
        return "info";
    case SENTRY_LEVEL_WARNING:
        return "warn";
    case SENTRY_LEVEL_ERROR:
        return "error";
    case SENTRY_LEVEL_FATAL:
        return "fatal";
    default:
        return "unknown";
    }
}

#ifndef SENTRY_UNITTEST
static
#endif
    sentry_value_t
    construct_param_from_conversion(
        const char conversion, printf_length_t length, va_list *args_copy)
{
    sentry_value_t param_obj = sentry_value_new_object();
    switch (conversion) {
    case 'd':
    case 'i': {
        int64_t val;
        switch (length) {
        case PRINTF_LENGTH_NONE:
            val = va_arg(*args_copy, int);
            break;
        case PRINTF_LENGTH_CHAR:
            val = (signed char)va_arg(*args_copy, int);
            break;
        case PRINTF_LENGTH_SHORT:
            val = (short)va_arg(*args_copy, int);
            break;
        case PRINTF_LENGTH_LONG:
            val = va_arg(*args_copy, long);
            break;
        case PRINTF_LENGTH_LONG_LONG:
            val = va_arg(*args_copy, long long);
            break;
        case PRINTF_LENGTH_INTMAX:
            val = va_arg(*args_copy, intmax_t);
            break;
        case PRINTF_LENGTH_SIZE:
            val = va_arg(*args_copy, printf_ssize_t);
            break;
        case PRINTF_LENGTH_PTRDIFF:
            val = va_arg(*args_copy, ptrdiff_t);
            break;
        case PRINTF_LENGTH_LONG_DOUBLE:
        default:
            val = va_arg(*args_copy, int);
            break;
        }
        sentry_value_set_by_key(
            param_obj, "value", sentry_value_new_int64(val));
        sentry_value_set_by_key(
            param_obj, "type", sentry_value_new_string("integer"));
        break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o': {
        uint64_t val;
        switch (length) {
        case PRINTF_LENGTH_NONE:
            val = va_arg(*args_copy, unsigned int);
            break;
        case PRINTF_LENGTH_CHAR:
            val = (unsigned char)va_arg(*args_copy, int);
            break;
        case PRINTF_LENGTH_SHORT:
            val = (unsigned short)va_arg(*args_copy, int);
            break;
        case PRINTF_LENGTH_LONG:
            val = va_arg(*args_copy, unsigned long);
            break;
        case PRINTF_LENGTH_LONG_LONG:
            val = va_arg(*args_copy, unsigned long long);
            break;
        case PRINTF_LENGTH_INTMAX:
            val = va_arg(*args_copy, uintmax_t);
            break;
        case PRINTF_LENGTH_SIZE:
            val = va_arg(*args_copy, size_t);
            break;
        case PRINTF_LENGTH_PTRDIFF:
            val = va_arg(*args_copy, printf_uptrdiff_t);
            break;
        case PRINTF_LENGTH_LONG_DOUBLE:
        default:
            val = va_arg(*args_copy, unsigned int);
            break;
        }
        // TODO update once unsigned 64-bit can be sent as non-string
        char buf[26];
        char format[8];
        snprintf(format, sizeof(format), "%%ll%c", conversion);
        snprintf(buf, sizeof(buf), format, (unsigned long long)val);
        sentry_value_set_by_key(
            param_obj, "value", sentry_value_new_string(buf));
        sentry_value_set_by_key(
            param_obj, "type", sentry_value_new_string("string"));
        break;
    }
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G': {
        double val = length == PRINTF_LENGTH_LONG_DOUBLE
            ? (double)va_arg(*args_copy, long double)
            : va_arg(*args_copy, double);
        sentry_value_set_by_key(
            param_obj, "value", sentry_value_new_double(val));
        sentry_value_set_by_key(
            param_obj, "type", sentry_value_new_string("double"));
        break;
    }
    case 'c': {
        int val = va_arg(*args_copy, int);
        char str[2] = { (char)val, '\0' };
        sentry_value_set_by_key(
            param_obj, "value", sentry_value_new_string(str));
        sentry_value_set_by_key(
            param_obj, "type", sentry_value_new_string("string"));
        break;
    }
    case 's': {
        const char *val = va_arg(*args_copy, const char *);
        if (val) {
            sentry_value_set_by_key(
                param_obj, "value", sentry_value_new_string(val));
        } else {
            sentry_value_set_by_key(
                param_obj, "value", sentry_value_new_string("(null)"));
        }
        sentry_value_set_by_key(
            param_obj, "type", sentry_value_new_string("string"));
        break;
    }
    case 'p': {
        void *val = va_arg(*args_copy, void *);
        char ptr_str[32];
        snprintf(ptr_str, sizeof(ptr_str), "%p", val);
        sentry_value_set_by_key(
            param_obj, "value", sentry_value_new_string(ptr_str));
        sentry_value_set_by_key(
            param_obj, "type", sentry_value_new_string("string"));
        break;
    }
    default:
        // Unknown format specifier, skip the argument
        (void)va_arg(*args_copy, void *);
        sentry_value_set_by_key(
            param_obj, "value", sentry_value_new_string("(unknown)"));
        sentry_value_set_by_key(
            param_obj, "type", sentry_value_new_string("string"));
        break;
    }

    return param_obj;
}

static const char *
skip_flags(const char *fmt_ptr)
{
    while (*fmt_ptr
        && (*fmt_ptr == '-' || *fmt_ptr == '+' || *fmt_ptr == ' '
            || *fmt_ptr == '#' || *fmt_ptr == '0')) {
        fmt_ptr++;
    }
    return fmt_ptr;
}

static const char *
skip_width(const char *fmt_ptr, va_list *args_copy)
{
    if (*fmt_ptr == '*') {
        (void)va_arg(*args_copy, int);
        return fmt_ptr + 1;
    }
    while (*fmt_ptr && (*fmt_ptr >= '0' && *fmt_ptr <= '9')) {
        fmt_ptr++;
    }
    return fmt_ptr;
}

static const char *
skip_precision(const char *fmt_ptr, va_list *args_copy)
{

    if (*fmt_ptr == '.') {
        fmt_ptr++;
        if (*fmt_ptr == '*') {
            (void)va_arg(*args_copy, int);
            return fmt_ptr + 1;
        }
        while (*fmt_ptr && (*fmt_ptr >= '0' && *fmt_ptr <= '9')) {
            fmt_ptr++;
        }
    }
    return fmt_ptr;
}

static const char *
parse_length(const char *fmt_ptr, printf_length_t *length)
{
    *length = PRINTF_LENGTH_NONE;
    switch (*fmt_ptr) {
    case 'h':
        if (*(fmt_ptr + 1) == 'h') {
            *length = PRINTF_LENGTH_CHAR;
            return fmt_ptr + 2;
        }
        *length = PRINTF_LENGTH_SHORT;
        return fmt_ptr + 1;
    case 'l':
        if (*(fmt_ptr + 1) == 'l') {
            *length = PRINTF_LENGTH_LONG_LONG;
            return fmt_ptr + 2;
        }
        *length = PRINTF_LENGTH_LONG;
        return fmt_ptr + 1;
    case 'L':
        *length = PRINTF_LENGTH_LONG_DOUBLE;
        return fmt_ptr + 1;
    case 'j':
        *length = PRINTF_LENGTH_INTMAX;
        return fmt_ptr + 1;
    case 'z':
        *length = PRINTF_LENGTH_SIZE;
        return fmt_ptr + 1;
    case 't':
        *length = PRINTF_LENGTH_PTRDIFF;
        return fmt_ptr + 1;
    default:
        return fmt_ptr;
    }
}

// returns how many parameters were added to the attributes object
#ifndef SENTRY_UNITTEST
static
#endif
    int
    populate_message_parameters(
        sentry_value_t attributes, const char *message, va_list args)
{
    if (!message || sentry_value_is_null(attributes)) {
        return 0;
    }

    const char *fmt_ptr = message;
    int param_index = 0;
    va_list args_copy;
    va_copy(args_copy, args);

    while (*fmt_ptr) {
        // Find the next format specifier
        if (*fmt_ptr == '%') {
            fmt_ptr++; // Skip the '%'

            if (*fmt_ptr == '%') {
                // Escaped '%', not a format specifier
                fmt_ptr++;
                continue;
            }

            fmt_ptr = skip_flags(fmt_ptr);
            fmt_ptr = skip_width(fmt_ptr, &args_copy);
            fmt_ptr = skip_precision(fmt_ptr, &args_copy);
            printf_length_t length;
            fmt_ptr = parse_length(fmt_ptr, &length);

            // Get the conversion specifier
            char conversion = *fmt_ptr;
            if (conversion) {
                char key[64];
                snprintf(key, sizeof(key), "sentry.message.parameter.%d",
                    param_index);
                sentry_value_t param_obj = construct_param_from_conversion(
                    conversion, length, &args_copy);
                sentry_value_set_by_key(attributes, key, param_obj);
                param_index++;
                fmt_ptr++;
            }
        } else {
            fmt_ptr++;
        }
    }

    va_end(args_copy);
    return param_index;
}

// Takes ownership of custom_attributes. Returns a new attributes object:
// cloned if object, empty otherwise.
static sentry_value_t
clone_attributes(sentry_value_t custom_attributes)
{
    sentry_value_t attributes;
    if (sentry_value_get_type(custom_attributes) == SENTRY_VALUE_TYPE_OBJECT) {
        attributes = sentry__value_clone(custom_attributes);
    } else {
        if (!sentry_value_is_null(custom_attributes)) {
            SENTRY_DEBUG("Discarded custom attributes on log: non-object "
                         "sentry_value_t passed in");
        }
        attributes = sentry_value_new_object();
    }
    sentry_value_decref(custom_attributes);
    return attributes;
}

static void
apply_attributes(
    sentry_value_t log, sentry_value_t attributes, sentry_level_t level)
{
    sentry_value_set_by_key(
        log, "level", sentry_value_new_string(level_as_string(level)));

    // timestamp in seconds
    uint64_t usec_time = sentry__usec_time();
    sentry_value_set_by_key(log, "timestamp",
        sentry_value_new_double((double)usec_time / 1000000.0));

    // adds data from the scope & options to the attributes, and adds `trace_id`
    // to the log
    sentry__apply_attributes(log, attributes);
    sentry_value_set_by_key(log, "attributes", attributes);
}

static sentry_value_t
construct_log(sentry_level_t level, const char *message, va_list args)
{
    sentry_value_t log = sentry_value_new_object();
    sentry_value_t attributes = sentry_value_new_object();

    SENTRY_WITH_OPTIONS (options) {
        // Extract custom attributes if the option is enabled
        if (sentry_options_get_logs_with_attributes(options)) {
            va_list args_copy;
            va_copy(args_copy, args);
            sentry_value_t custom_attributes
                = va_arg(args_copy, sentry_value_t);
            va_end(args_copy);
            sentry_value_decref(attributes);
            attributes = clone_attributes(custom_attributes);
        }

        // Format the message with remaining args (or all args if not using
        // custom attributes)
        va_list args_copy_1, args_copy_2, args_copy_3;
        va_copy(args_copy_1, args);
        va_copy(args_copy_2, args);
        va_copy(args_copy_3, args);

        // Skip the first argument (attributes) if using custom attributes
        if (sentry_options_get_logs_with_attributes(options)) {
            va_arg(args_copy_1, sentry_value_t);
            va_arg(args_copy_2, sentry_value_t);
            va_arg(args_copy_3, sentry_value_t);
        }

        int len = vsnprintf(NULL, 0, message, args_copy_1) + 1;
        va_end(args_copy_1);
        size_t size = (size_t)len;
        char *fmt_message = sentry_malloc(size);
        if (!fmt_message) {
            va_end(args_copy_2);
            va_end(args_copy_3);
            return sentry_value_new_null();
        }

        vsnprintf(fmt_message, size, message, args_copy_2);
        va_end(args_copy_2);

        sentry_value_set_by_key(
            log, "body", sentry_value_new_string(fmt_message));
        sentry_free(fmt_message);

        // Parse variadic arguments and add them to attributes
        if (populate_message_parameters(attributes, message, args_copy_3)) {
            // only add message template if we have parameters
            sentry__value_add_attribute(attributes,
                sentry_value_new_string(message), "string",
                "sentry.message.template");
        }
        va_end(args_copy_3);
    }

    apply_attributes(log, attributes, level);

    return log;
}

static void
debug_print_log(sentry_level_t level, const char *log_body)
{
    // TODO if we enable our debug-macro as logging integration
    //  we need to avoid recursion here
    switch (level) {
    case SENTRY_LEVEL_TRACE:
        SENTRY_TRACEF("LOG: %s", log_body);
        break;
    case SENTRY_LEVEL_DEBUG:
        SENTRY_DEBUGF("LOG: %s", log_body);
        break;
    case SENTRY_LEVEL_INFO:
        SENTRY_INFOF("LOG: %s", log_body);
        break;
    case SENTRY_LEVEL_WARNING:
        SENTRY_WARNF("LOG: %s", log_body);
        break;
    case SENTRY_LEVEL_ERROR:
        SENTRY_ERRORF("LOG: %s", log_body);
        break;
    case SENTRY_LEVEL_FATAL:
        SENTRY_FATALF("LOG: %s", log_body);
        break;
    }
}

static log_return_value_t
send_log(sentry_level_t level, sentry_value_t log)
{
    bool discarded = false;
    SENTRY_WITH_OPTIONS (options) {
        if (options->before_send_log_func) {
            log = options->before_send_log_func(
                log, options->before_send_log_data);
            if (sentry_value_is_null(log)) {
                SENTRY_DEBUG("log was discarded by the `before_send_log` hook");
                sentry__client_report_discard(SENTRY_DISCARD_REASON_BEFORE_SEND,
                    SENTRY_DATA_CATEGORY_LOG_ITEM, 1);
                discarded = true;
            }
        }
        if (options->debug && !sentry_value_is_null(log)) {
            debug_print_log(level,
                sentry_value_as_string(sentry_value_get_by_key(log, "body")));
        }
    }
    if (discarded) {
        return SENTRY_LOG_RETURN_DISCARD;
    }
    sentry_batcher_t *batcher = sentry__batcher_acquire(&g_batcher);
    if (!batcher || !sentry__batcher_enqueue(batcher, log)) {
        sentry__batcher_release(batcher);
        sentry_value_decref(log);
        return SENTRY_LOG_RETURN_FAILED;
    }
    sentry__batcher_release(batcher);
    return SENTRY_LOG_RETURN_SUCCESS;
}

log_return_value_t
sentry__logs_log(sentry_level_t level, const char *message, va_list args)
{
    bool enable_logs = false;
    SENTRY_WITH_OPTIONS (options) {
        if (options->enable_logs)
            enable_logs = true;
    }
    if (!enable_logs) {
        return SENTRY_LOG_RETURN_DISABLED;
    }
    return send_log(level, construct_log(level, message, args));
}

log_return_value_t
sentry_log_trace(const char *message, ...)
{
    va_list args;
    va_start(args, message);
    log_return_value_t result
        = sentry__logs_log(SENTRY_LEVEL_TRACE, message, args);
    va_end(args);
    return result;
}

log_return_value_t
sentry_log_debug(const char *message, ...)
{
    va_list args;
    va_start(args, message);
    const log_return_value_t result
        = sentry__logs_log(SENTRY_LEVEL_DEBUG, message, args);
    va_end(args);
    return result;
}

log_return_value_t
sentry_log_info(const char *message, ...)
{
    va_list args;
    va_start(args, message);
    const log_return_value_t result
        = sentry__logs_log(SENTRY_LEVEL_INFO, message, args);
    va_end(args);
    return result;
}

log_return_value_t
sentry_log_warn(const char *message, ...)
{
    va_list args;
    va_start(args, message);
    const log_return_value_t result
        = sentry__logs_log(SENTRY_LEVEL_WARNING, message, args);
    va_end(args);
    return result;
}

log_return_value_t
sentry_log_error(const char *message, ...)
{
    va_list args;
    va_start(args, message);
    const log_return_value_t result
        = sentry__logs_log(SENTRY_LEVEL_ERROR, message, args);
    va_end(args);
    return result;
}

log_return_value_t
sentry_log_fatal(const char *message, ...)
{
    va_list args;
    va_start(args, message);
    const log_return_value_t result
        = sentry__logs_log(SENTRY_LEVEL_FATAL, message, args);
    va_end(args);
    return result;
}

log_return_value_t
sentry_log(
    sentry_level_t level, const char *body, sentry_value_t custom_attributes)
{
    bool enable_logs = false;
    SENTRY_WITH_OPTIONS (options) {
        if (options->enable_logs)
            enable_logs = true;
    }
    if (!enable_logs) {
        sentry_value_decref(custom_attributes);
        return SENTRY_LOG_RETURN_DISABLED;
    }

    sentry_value_t log = sentry_value_new_object();
    sentry_value_t attributes = clone_attributes(custom_attributes);

    sentry_value_set_by_key(log, "body", sentry_value_new_string(body));
    apply_attributes(log, attributes, level);

    return send_log(level, log);
}

void
sentry__logs_startup(const sentry_options_t *options)
{
    sentry_batcher_t *batcher = sentry__batcher_new(
        sentry__envelope_add_logs, SENTRY_DATA_CATEGORY_LOG_ITEM);
    if (!batcher) {
        SENTRY_WARN("failed to allocate logs batcher");
        return;
    }

    sentry__batcher_startup(batcher, options);
    sentry_batcher_t *old = sentry__batcher_swap(&g_batcher, batcher);

    if (old) {
        sentry__batcher_shutdown(old, 0);
    }
    sentry__batcher_release(old);
}

void
sentry__logs_shutdown(uint64_t timeout)
{
    SENTRY_DEBUG("shutting down logs system");
    sentry_batcher_t *batcher = sentry__batcher_swap(&g_batcher, NULL);
    if (batcher) {
        sentry__batcher_shutdown(batcher, timeout);
        sentry__batcher_release(batcher);
    }
    SENTRY_DEBUG("logs system shutdown complete");
}

void
sentry__logs_flush_crash_safe(void)
{
    SENTRY_SIGNAL_SAFE_LOG("DEBUG crash-safe logs flush");
    sentry_batcher_t *batcher = sentry__batcher_peek(&g_batcher);
    if (batcher) {
        sentry__batcher_flush_crash_safe(batcher);
    }
    SENTRY_SIGNAL_SAFE_LOG("DEBUG crash-safe logs flush complete");
}

uintptr_t
sentry__logs_force_flush_begin(void)
{
    sentry_batcher_t *batcher = sentry__batcher_acquire(&g_batcher);
    if (batcher) {
        sentry__batcher_force_flush_begin(batcher);
    }
    return (uintptr_t)batcher;
}

void
sentry__logs_force_flush_wait(uintptr_t token)
{
    sentry_batcher_t *batcher = (sentry_batcher_t *)token;
    if (batcher) {
        sentry__batcher_force_flush_wait(batcher);
        sentry__batcher_release(batcher);
    }
}

#ifdef SENTRY_UNITTEST
/**
 * Wait for the logs batching thread to be ready.
 * This is a test-only helper to avoid race conditions in tests.
 */
void
sentry__logs_wait_for_thread_startup(void)
{
    sentry_batcher_t *batcher = sentry__batcher_acquire(&g_batcher);
    if (batcher) {
        sentry__batcher_wait_for_thread_startup(batcher);
        sentry__batcher_release(batcher);
    }
}
#endif
