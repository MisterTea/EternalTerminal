#include "sentry_logs.h"
#include "sentry_sync.h"
#include "sentry_testsupport.h"

#include "sentry_envelope.h"
#include <string.h>

typedef struct {
    volatile long called_count;
    bool has_validation_error;
} transport_validation_data_t;

static void
validate_logs_envelope(sentry_envelope_t *envelope, void *data)
{
    transport_validation_data_t *validation_data = data;

    // Check we have at least one envelope item (store error flag instead of
    // TEST_CHECK)
    if (sentry__envelope_get_item_count(envelope) == 0) {
        validation_data->has_validation_error = true;
        sentry_envelope_free(envelope);
        return;
    }

    // Get the first item and check its type
    const sentry_envelope_item_t *item = sentry__envelope_get_item(envelope, 0);
    sentry_value_t type_header = sentry__envelope_item_get_header(item, "type");
    const char *type = sentry_value_as_string(type_header);

    // Only validate and count log envelopes, skip others (e.g., session)
    if (strcmp(type, "log") == 0) {
        sentry__atomic_fetch_and_add(&validation_data->called_count, 1);
    }

    sentry_envelope_free(envelope);
}

SENTRY_TEST(basic_logging_functionality)
{
    transport_validation_data_t validation_data = { 0, false };

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");

    sentry_transport_t *transport
        = sentry_transport_new(validate_logs_envelope);
    sentry_transport_set_state(transport, &validation_data);
    sentry_options_set_transport(options, transport);

    sentry_init(options);
    sentry__logs_wait_for_thread_startup();

    // These should not crash and should respect the enable_logs option
    TEST_CHECK_INT_EQUAL(sentry_log_trace("Trace message"), 0);
    TEST_CHECK_INT_EQUAL(sentry_log_debug("Debug message"), 0);
    TEST_CHECK_INT_EQUAL(sentry_log_info("Info message"), 0);
    TEST_CHECK_INT_EQUAL(sentry_log_warn("Warning message"), 0);
    TEST_CHECK_INT_EQUAL(sentry_log_error("Error message"), 0);
    // Sleep up to 5s to allow first batch to flush (testing batch timing
    // behavior)
    for (int i = 0;
        i < 250 && sentry__atomic_fetch(&validation_data.called_count) < 1;
        i++) {
        sleep_ms(20);
    }
    TEST_CHECK_INT_EQUAL(validation_data.called_count, 1);
    TEST_CHECK_INT_EQUAL(sentry_log_fatal("Fatal message"), 0);
    sentry_close();

    // Validate results on main thread (no race condition)
    TEST_CHECK(!validation_data.has_validation_error);
    TEST_CHECK_INT_EQUAL(validation_data.called_count, 2);
}

SENTRY_TEST(logs_disabled)
{
    transport_validation_data_t validation_data = { 0, false };

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_enable_logs(options, false);

    sentry_transport_t *transport
        = sentry_transport_new(validate_logs_envelope);
    sentry_transport_set_state(transport, &validation_data);
    sentry_options_set_transport(options, transport);

    sentry_init(options);

    // Should return DISABLED since logs were explicitly disabled
    TEST_CHECK_INT_EQUAL(sentry_log_info("This should not be sent"), 3);

    sentry_close();

    // Transport should not be called since logs were explicitly disabled
    TEST_CHECK(!validation_data.has_validation_error);
    TEST_CHECK_INT_EQUAL(validation_data.called_count, 0);
}

SENTRY_TEST(formatted_log_messages)
{
    transport_validation_data_t validation_data = { 0, false };

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");

    sentry_transport_t *transport
        = sentry_transport_new(validate_logs_envelope);
    sentry_transport_set_state(transport, &validation_data);
    sentry_options_set_transport(options, transport);

    sentry_init(options);
    sentry__logs_wait_for_thread_startup();

    // Test format specifiers
    TEST_CHECK_INT_EQUAL(sentry_log_info("String: %s, Integer: %d, Float: %.2f",
                             "test", 42, 3.14),
        0);
    TEST_CHECK_INT_EQUAL(
        sentry_log_warn("Character: %c, Hex: 0x%x", 'A', 255), 0);
    TEST_CHECK_INT_EQUAL(sentry_log_error("Pointer: %p", (void *)0x1234), 0);
    TEST_CHECK_INT_EQUAL(sentry_log_error("Big number: %zu", UINT64_MAX), 0);
    TEST_CHECK_INT_EQUAL(
        sentry_log_error("Small number: %" PRId64, INT64_MIN), 0);

    sentry_close();

    // Transport should be called once
    TEST_CHECK(!validation_data.has_validation_error);
    TEST_CHECK_INT_EQUAL(validation_data.called_count, 1);
}

static void
test_param_conversion_helper(const char *format, ...)
{
    sentry_value_t attributes = sentry_value_new_object();
    va_list args;
    va_start(args, format);
    int param_count = populate_message_parameters(attributes, format, args);
    va_end(args);

    // Verify we got the expected number of parameters
    TEST_CHECK_INT_EQUAL(param_count, 3);

    // Verify the parameters were extracted correctly
    sentry_value_t param0
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.0");
    sentry_value_t param1
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.1");
    sentry_value_t param2
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.2");

    TEST_CHECK(!sentry_value_is_null(param0));
    TEST_CHECK(!sentry_value_is_null(param1));
    TEST_CHECK(!sentry_value_is_null(param2));

    // Check the values
    sentry_value_t value0 = sentry_value_get_by_key(param0, "value");
    sentry_value_t value1 = sentry_value_get_by_key(param1, "value");
    sentry_value_t value2 = sentry_value_get_by_key(param2, "value");

    TEST_CHECK_INT_EQUAL(sentry_value_as_int64(value0), 1);
    TEST_CHECK_INT_EQUAL(sentry_value_as_int64(value1), 2);
    TEST_CHECK_INT_EQUAL(sentry_value_as_int64(value2), 3);

    sentry_value_decref(attributes);
}

static int
populate_test_params(sentry_value_t attributes, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int param_count = populate_message_parameters(attributes, format, args);
    va_end(args);
    return param_count;
}

SENTRY_TEST(logs_param_conversion)
{
    int a = 1, b = 2, c = 3;
    test_param_conversion_helper("%d %d %d", a, b, c);
}

SENTRY_TEST(logs_param_sign)
{
    sentry_value_t attributes = sentry_value_new_object();
    int value = -1;

    int param_count = populate_test_params(attributes, "%d", value);

    sentry_value_t param
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.0");
    sentry_value_t param_value = sentry_value_get_by_key(param, "value");

    TEST_CHECK_INT_EQUAL(param_count, 1);
    TEST_CHECK_INT_EQUAL(sentry_value_as_int64(param_value), -1);

    sentry_value_decref(attributes);
}

SENTRY_TEST(logs_param_width)
{
    sentry_value_t attributes = sentry_value_new_object();
    const char *format = "%*d";

    int param_count = populate_test_params(attributes, format, 6, 42);

    sentry_value_t param
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.0");
    sentry_value_t param_value = sentry_value_get_by_key(param, "value");

    TEST_CHECK_INT_EQUAL(param_count, 1);
    TEST_CHECK_INT_EQUAL(sentry_value_as_int64(param_value), 42);

    sentry_value_decref(attributes);
}

static void
test_param_conversion_types(const char *format, ...)
{
    sentry_value_t attributes = sentry_value_new_object();
    va_list args;
    va_start(args, format);
    int param_count = populate_message_parameters(attributes, format, args);
    va_end(args);

    // Verify we got the expected number of parameters
    TEST_CHECK_INT_EQUAL(param_count, 7);

    // Verify the parameters were extracted correctly
    sentry_value_t param0
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.0");
    sentry_value_t param1
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.1");
    sentry_value_t param2
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.2");
    sentry_value_t param3
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.3");
    sentry_value_t param4
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.4");
    sentry_value_t param5
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.5");
    sentry_value_t param6
        = sentry_value_get_by_key(attributes, "sentry.message.parameter.6");

    TEST_CHECK(!sentry_value_is_null(param0));
    TEST_CHECK(!sentry_value_is_null(param1));
    TEST_CHECK(!sentry_value_is_null(param2));
    TEST_CHECK(!sentry_value_is_null(param3));
    TEST_CHECK(!sentry_value_is_null(param4));
    TEST_CHECK(!sentry_value_is_null(param5));
    TEST_CHECK(!sentry_value_is_null(param6));

    // Check the values and types
    sentry_value_t value0 = sentry_value_get_by_key(param0, "value");
    sentry_value_t value1 = sentry_value_get_by_key(param1, "value");
    sentry_value_t value2 = sentry_value_get_by_key(param2, "value");
    sentry_value_t value3 = sentry_value_get_by_key(param3, "value");
    sentry_value_t value4 = sentry_value_get_by_key(param4, "value");
    sentry_value_t value5 = sentry_value_get_by_key(param5, "value");
    sentry_value_t value6 = sentry_value_get_by_key(param6, "value");

    sentry_value_t type0 = sentry_value_get_by_key(param0, "type");
    sentry_value_t type1 = sentry_value_get_by_key(param1, "type");
    sentry_value_t type2 = sentry_value_get_by_key(param2, "type");
    sentry_value_t type3 = sentry_value_get_by_key(param3, "type");
    sentry_value_t type4 = sentry_value_get_by_key(param4, "type");
    sentry_value_t type5 = sentry_value_get_by_key(param5, "type");
    sentry_value_t type6 = sentry_value_get_by_key(param6, "type");

    // Validate %u (unsigned) - should be string type with UINT64_MAX value
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(type0), "string");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(value0), "18446744073709551615");

    // Validate %d (signed integer) - should be integer type with INT64_MIN
    // value
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(type1), "integer");
    TEST_CHECK_INT_EQUAL(sentry_value_as_int64(value1), INT64_MIN);

    // Validate %f (float) - should be double type with 3.14159 value
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(type2), "double");
    TEST_CHECK(sentry_value_as_double(value2) == 3.14159);

    // Validate %c (character) - should be string type with 'A' value
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(type3), "string");
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(value3), "A");

    // Validate %s (string) - should be string type with "test" value
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(type4), "string");
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(value4), "test");

    // Validate %p (pointer) - should be string type with pointer representation
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(type5), "string");
    // Pointer value format is platform-dependent, but should contain the hex
    // digits (can be upper- or lower-cased)
    const char *ptr_str = sentry_value_as_string(value5);
    TEST_CHECK(ptr_str != NULL);
    TEST_CHECK(strstr(ptr_str, "12345abc") != NULL
        || strstr(ptr_str, "12345ABC") != NULL);

    // Validate %x (hex uint64) - should be string type with hex representation
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(type6), "string");
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(value6), "deadbeefdeadbeef");

    sentry_value_decref(attributes);
}

SENTRY_TEST(logs_param_types)
{
    uint64_t a = UINT64_MAX;
    int64_t b = INT64_MIN;
    double c = 3.14159;
    char d = 'A';
    const char *e = "test";
    void *f = (void *)0x12345abc;
    uint64_t g = 0xDEADBEEFDEADBEEF;
    test_param_conversion_types(
        "%" PRIu64 " %" PRId64 " %f %c %s %p %" PRIx64, a, b, c, d, e, f, g);
}

SENTRY_TEST(logs_force_flush)
{
    transport_validation_data_t validation_data = { 0, false };

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");

    sentry_transport_t *transport
        = sentry_transport_new(validate_logs_envelope);
    sentry_transport_set_state(transport, &validation_data);
    sentry_options_set_transport(options, transport);

    sentry_init(options);
    sentry__logs_wait_for_thread_startup();

    // These should not crash and should respect the enable_logs option
    TEST_CHECK_INT_EQUAL(sentry_log_trace("Trace message"), 0);
    sentry_flush(5000);
    TEST_CHECK_INT_EQUAL(sentry_log_debug("Debug message"), 0);
    sentry_flush(5000);
    TEST_CHECK_INT_EQUAL(sentry_log_info("Info message"), 0);
    sentry_flush(5000);
    TEST_CHECK_INT_EQUAL(sentry_log_warn("Warning message"), 0);
    sentry_flush(5000);
    TEST_CHECK_INT_EQUAL(sentry_log_error("Error message"), 0);
    sentry_flush(5000);
    TEST_CHECK_INT_EQUAL(sentry_log_fatal("Fatal message"), 0);
    sentry_flush(5000);
    sentry_close();

    // Validate results on main thread (no race condition)
    TEST_CHECK(!validation_data.has_validation_error);
    TEST_CHECK_INT_EQUAL(validation_data.called_count, 6);
}

SENTRY_TEST(logs_custom_attributes_with_format_strings)
{
    transport_validation_data_t validation_data = { 0, false };

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_logs_with_attributes(options, true);

    sentry_transport_t *transport
        = sentry_transport_new(validate_logs_envelope);
    sentry_transport_set_state(transport, &validation_data);
    sentry_options_set_transport(options, transport);

    sentry_init(options);
    sentry__logs_wait_for_thread_startup();

    // Test 1: Custom attributes with format string
    sentry_value_t attributes1 = sentry_value_new_object();
    sentry_value_t attr1 = sentry_value_new_attribute(
        sentry_value_new_string("custom_value"), NULL);
    sentry_value_set_by_key(attributes1, "my.custom.attribute", attr1);
    TEST_CHECK_INT_EQUAL(sentry_log_info("User %s logged in with code %d",
                             attributes1, "Alice", 200),
        0);

    // Test 2: Null attributes with format string (should still work)
    TEST_CHECK_INT_EQUAL(sentry_log_warn("No custom attrs: %s has %d items",
                             sentry_value_new_null(), "cart", 5),
        0);

    // Test 3: Custom attributes with no format parameters
    sentry_value_t attributes2 = sentry_value_new_object();
    sentry_value_t attr2
        = sentry_value_new_attribute(sentry_value_new_int32(42), NULL);
    sentry_value_set_by_key(attributes2, "special.number", attr2);
    TEST_CHECK_INT_EQUAL(
        sentry_log_error("Simple message with custom attrs", attributes2), 0);

    // Test 4: Custom attributes with multiple format types
    sentry_value_t attributes3 = sentry_value_new_object();
    sentry_value_t attr3
        = sentry_value_new_attribute(sentry_value_new_string("tracking"), NULL);
    sentry_value_set_by_key(attributes3, "event.type", attr3);
    TEST_CHECK_INT_EQUAL(
        sentry_log_debug("Processing item %d of %d (%.1f%% complete)",
            attributes3, 3, 10, 30.0),
        0);

    sentry_close();

    // Validate that logs were sent
    TEST_CHECK(!validation_data.has_validation_error);
    TEST_CHECK_INT_EQUAL(validation_data.called_count, 1);
}

SENTRY_TEST(logs_custom_attributes_not_modified)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_logs_with_attributes(options, true);
    sentry_init(options);

    sentry_set_attribute("global.key",
        sentry_value_new_attribute(
            sentry_value_new_string("global_value"), NULL));

    sentry_value_t attrs = sentry_value_new_object();
    sentry_value_set_by_key(attrs, "local.key",
        sentry_value_new_attribute(
            sentry_value_new_string("local_value"), NULL));
    sentry_value_incref(attrs);

    sentry_log_info("Test message", attrs);

    // attrs should still contain only local.key, not global.key
    TEST_CHECK_INT_EQUAL(sentry_value_get_length(attrs), 1);
    TEST_CHECK(
        !sentry_value_is_null(sentry_value_get_by_key(attrs, "local.key")));
    TEST_CHECK(
        sentry_value_is_null(sentry_value_get_by_key(attrs, "global.key")));

    sentry_value_decref(attrs);
    sentry_close();
}

static sentry_value_t
capture_log_before_send(sentry_value_t log, void *data)
{
    sentry_value_t *captured = data;
    sentry_value_decref(*captured);
    sentry_value_incref(log);
    *captured = log;
    return log;
}

SENTRY_TEST(logs_plain_string)
{
    sentry_value_t captured_log = sentry_value_new_null();

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_before_send_log(
        options, capture_log_before_send, &captured_log);

    sentry_transport_t *transport
        = sentry_transport_new(validate_logs_envelope);
    transport_validation_data_t validation_data = { 0, false };
    sentry_transport_set_state(transport, &validation_data);
    sentry_options_set_transport(options, transport);

    sentry_init(options);
    sentry__logs_wait_for_thread_startup();

    // Body with printf-dangerous characters stored literally
    sentry_value_t attrs = sentry_value_new_object();
    sentry_value_set_by_key(attrs, "my.key",
        sentry_value_new_attribute(sentry_value_new_string("my_value"), NULL));
    TEST_CHECK_INT_EQUAL(
        sentry_log(SENTRY_LEVEL_INFO, "100% done %n %s", attrs),
        SENTRY_LOG_RETURN_SUCCESS);

    // Verify body is stored literally
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(captured_log, "body")),
        "100% done %n %s");

    // Verify level
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(captured_log, "level")),
        "info");

    // Verify custom attribute is present
    sentry_value_t log_attrs
        = sentry_value_get_by_key(captured_log, "attributes");
    TEST_CHECK(
        !sentry_value_is_null(sentry_value_get_by_key(log_attrs, "my.key")));

    // Verify no message template or parameters
    TEST_CHECK(sentry_value_is_null(
        sentry_value_get_by_key(log_attrs, "sentry.message.template")));
    TEST_CHECK(sentry_value_is_null(
        sentry_value_get_by_key(log_attrs, "sentry.message.parameter.0")));

    // Test with null attributes
    TEST_CHECK_INT_EQUAL(sentry_log(SENTRY_LEVEL_ERROR, "another %d message",
                             sentry_value_new_null()),
        SENTRY_LOG_RETURN_SUCCESS);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(captured_log, "body")),
        "another %d message");

    sentry_value_decref(captured_log);
    sentry_close();

    TEST_CHECK(!validation_data.has_validation_error);
}

SENTRY_TEST(logs_plain_string_disabled)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_enable_logs(options, false);

    sentry_init(options);

    // Should not leak the attributes value
    sentry_value_t attrs = sentry_value_new_object();
    sentry_value_set_by_key(attrs, "key",
        sentry_value_new_attribute(sentry_value_new_string("val"), NULL));
    TEST_CHECK_INT_EQUAL(sentry_log(SENTRY_LEVEL_INFO, "test", attrs),
        SENTRY_LOG_RETURN_DISABLED);

    sentry_close();
}

SENTRY_TEST(logs_reinit)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");

    sentry_init(options);
    sentry__logs_wait_for_thread_startup();

    // Fill the buffer to trigger a flush on the batcher thread
    for (int i = 0; i < 5; i++) {
        sentry_log_info("log %d", i);
    }

    // Re-init immediately while the batcher thread is likely mid-flush.
    // This will deadlock if sentry__batcher_flush holds g_options_lock.
    SENTRY_TEST_OPTIONS_NEW(options2);
    sentry_options_set_dsn(options2, "https://foo@sentry.invalid/42");

    sentry_init(options2);
    sentry_close();
}

SENTRY_THREAD_FN
log_producer_thread(void *data)
{
    volatile long *produce = data;
    while (sentry__atomic_fetch(produce)) {
        sentry_log_info("log from producer thread");
        sentry__thread_yield();
    }
    return 0;
}

// multiple threads produce logs during SDK re-init
SENTRY_TEST(logs_reinit_stress)
{
    volatile long produce = 1;

    sentry_threadid_t threads[8];
    for (int t = 0; t < 8; t++) {
        sentry__thread_init(&threads[t]);
    }

    for (int i = 0; i < 5; i++) {
        SENTRY_TEST_OPTIONS_NEW(options);
        sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
        sentry_init(options);

        if (i == 0) {
            sentry__logs_wait_for_thread_startup();
            for (int t = 0; t < 8; t++) {
                sentry__thread_spawn(
                    &threads[t], log_producer_thread, (void *)&produce);
            }
        }

        sentry_close();
    }

    sentry__atomic_store(&produce, 0);
    for (int t = 0; t < 8; t++) {
        sentry__thread_join(threads[t]);
    }
}
