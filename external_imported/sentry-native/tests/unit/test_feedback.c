#include "sentry_attachment.h"
#include "sentry_envelope.h"
#include "sentry_hint.h"
#include "sentry_path.h"
#include "sentry_testsupport.h"
#include "sentry_transport.h"

typedef struct {
    uint64_t called;
    sentry_stringbuilder_t serialized_envelope;
} sentry_feedback_testdata_t;

static void
send_envelope_test_feedback(sentry_envelope_t *envelope, void *_data)
{
    sentry_feedback_testdata_t *data = _data;
    data->called += 1;
    sentry__envelope_serialize_into_stringbuilder(
        envelope, &data->serialized_envelope);
    sentry_envelope_free(envelope);
}

static void
setup_feedback_test(sentry_feedback_testdata_t *testdata)
{
    testdata->called = 0;
    sentry__stringbuilder_init(&testdata->serialized_envelope);

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_auto_session_tracking(options, false);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_transport_t *transport
        = sentry_transport_new(send_envelope_test_feedback);
    sentry_transport_set_state(transport, testdata);
    sentry_options_set_transport(options, transport);

    sentry_init(options);
}

SENTRY_TEST(feedback_without_hint)
{
    sentry_feedback_testdata_t testdata;
    setup_feedback_test(&testdata);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("4c035723-8638-4c3a-923f-2ab9d08b4018");
    sentry_value_t feedback = sentry_value_new_feedback(
        "test message", "test@example.com", "Test User", &event_id);

    sentry_capture_feedback(feedback);

    char *serialized
        = sentry_stringbuilder_take_string(&testdata.serialized_envelope);
    TEST_CHECK(strstr(serialized, "\"type\":\"feedback\"") != NULL);
    TEST_CHECK(strstr(serialized, "\"type\":\"attachment\"") == NULL);
    sentry_free(serialized);

    sentry_close();

    TEST_CHECK_INT_EQUAL(testdata.called, 1);
}

SENTRY_TEST(feedback_with_null_hint)
{
    sentry_feedback_testdata_t testdata;
    setup_feedback_test(&testdata);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("4c035723-8638-4c3a-923f-2ab9d08b4018");
    sentry_value_t feedback = sentry_value_new_feedback(
        "test message", "test@example.com", "Test User", &event_id);

    sentry_capture_feedback_with_hint(feedback, NULL);

    char *serialized
        = sentry_stringbuilder_take_string(&testdata.serialized_envelope);
    TEST_CHECK(strstr(serialized, "\"type\":\"feedback\"") != NULL);
    TEST_CHECK(strstr(serialized, "\"type\":\"attachment\"") == NULL);
    sentry_free(serialized);

    sentry_close();

    TEST_CHECK_INT_EQUAL(testdata.called, 1);
}

SENTRY_TEST(feedback_with_file_attachment)
{
    sentry_feedback_testdata_t testdata;
    setup_feedback_test(&testdata);

    sentry_path_t *attachment_path
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".feedback-attachment");
    sentry__path_write_buffer(attachment_path, "feedback data", 13);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("4c035723-8638-4c3a-923f-2ab9d08b4018");
    sentry_value_t feedback = sentry_value_new_feedback(
        "test message", "test@example.com", "Test User", &event_id);

    sentry_hint_t *hint = sentry_hint_new();
    TEST_CHECK(hint != NULL);

    sentry_attachment_t *attachment = sentry_hint_attach_file(
        hint, SENTRY_TEST_PATH_PREFIX ".feedback-attachment");
    TEST_CHECK(attachment != NULL);

    sentry_capture_feedback_with_hint(feedback, hint);

    char *serialized
        = sentry_stringbuilder_take_string(&testdata.serialized_envelope);
    TEST_CHECK(strstr(serialized, "\"type\":\"feedback\"") != NULL);
    TEST_CHECK(strstr(serialized,
                   "{\"type\":\"attachment\",\"length\":13,"
                   "\"filename\":\".feedback-attachment\"}\n"
                   "feedback data")
        != NULL);
    sentry_free(serialized);

    sentry_close();

    sentry__path_remove(attachment_path);
    sentry__path_free(attachment_path);

    TEST_CHECK_INT_EQUAL(testdata.called, 1);
}

SENTRY_TEST(feedback_with_bytes_attachment)
{
    sentry_feedback_testdata_t testdata;
    setup_feedback_test(&testdata);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("4c035723-8638-4c3a-923f-2ab9d08b4018");
    sentry_value_t feedback = sentry_value_new_feedback(
        "test message", "test@example.com", "Test User", &event_id);

    sentry_hint_t *hint = sentry_hint_new();
    TEST_CHECK(hint != NULL);

    const char binary_data[] = "binary\0data\0here";
    sentry_attachment_t *attachment = sentry_hint_attach_bytes(
        hint, binary_data, sizeof(binary_data) - 1, "binary.dat");
    TEST_CHECK(attachment != NULL);

    sentry_capture_feedback_with_hint(feedback, hint);

    char *serialized
        = sentry_stringbuilder_take_string(&testdata.serialized_envelope);
    TEST_CHECK(strstr(serialized, "\"type\":\"feedback\"") != NULL);
    TEST_CHECK(strstr(serialized,
                   "{\"type\":\"attachment\",\"length\":16,"
                   "\"filename\":\"binary.dat\"}")
        != NULL);
    TEST_CHECK(strstr(serialized, "binary") != NULL);
    sentry_free(serialized);

    sentry_close();

    TEST_CHECK_INT_EQUAL(testdata.called, 1);
}

SENTRY_TEST(feedback_with_multiple_attachments)
{
    sentry_feedback_testdata_t testdata;
    setup_feedback_test(&testdata);

    sentry_path_t *file1
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".feedback-file1");
    sentry_path_t *file2
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".feedback-file2");
    sentry__path_write_buffer(file1, "file1 content", 13);
    sentry__path_write_buffer(file2, "file2 content", 13);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("4c035723-8638-4c3a-923f-2ab9d08b4018");
    sentry_value_t feedback = sentry_value_new_feedback(
        "test message", "test@example.com", "Test User", &event_id);

    sentry_hint_t *hint = sentry_hint_new();
    TEST_CHECK(hint != NULL);

    sentry_attachment_t *attachment1 = sentry_hint_attach_file(
        hint, SENTRY_TEST_PATH_PREFIX ".feedback-file1");
    TEST_CHECK(attachment1 != NULL);

    sentry_attachment_t *attachment2
        = sentry_hint_attach_bytes(hint, "bytes content", 13, "bytes.txt");
    TEST_CHECK(attachment2 != NULL);

    sentry_attachment_t *attachment3 = sentry_hint_attach_file(
        hint, SENTRY_TEST_PATH_PREFIX ".feedback-file2");
    TEST_CHECK(attachment3 != NULL);

    sentry_capture_feedback_with_hint(feedback, hint);

    char *serialized
        = sentry_stringbuilder_take_string(&testdata.serialized_envelope);
    TEST_CHECK(strstr(serialized, "\"type\":\"feedback\"") != NULL);
    TEST_CHECK(strstr(serialized, "\"filename\":\".feedback-file1\"") != NULL);
    TEST_CHECK(strstr(serialized, "\"filename\":\"bytes.txt\"") != NULL);
    TEST_CHECK(strstr(serialized, "\"filename\":\".feedback-file2\"") != NULL);
    TEST_CHECK(strstr(serialized, "file1 content") != NULL);
    TEST_CHECK(strstr(serialized, "bytes content") != NULL);
    TEST_CHECK(strstr(serialized, "file2 content") != NULL);
    sentry_free(serialized);

    sentry_close();

    sentry__path_remove(file1);
    sentry__path_remove(file2);
    sentry__path_free(file1);
    sentry__path_free(file2);

    TEST_CHECK_INT_EQUAL(testdata.called, 1);
}
