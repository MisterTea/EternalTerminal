#include "sentry_attachment.h"
#include "sentry_client_report.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_json.h"
#include "sentry_options.h"
#include "sentry_path.h"
#include "sentry_ratelimiter.h"
#include "sentry_string.h"
#include "sentry_testsupport.h"
#include "sentry_utils.h"
#include "sentry_value.h"
#include "transports/sentry_http_transport.h"

#if defined(SENTRY_PLATFORM_UNIX) && !defined(SENTRY_PLATFORM_ANDROID)         \
    && !defined(SENTRY_PLATFORM_PS) && !defined(SENTRY_PLATFORM_NX)
#    include <signal.h>
#    include <sys/resource.h>
#    include <sys/wait.h>
#endif

static char *const SERIALIZED_ENVELOPE_STR
    = "{\"dsn\":\"https://foo@sentry.invalid/42\","
      "\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\",\"trace\":{"
      "\"public_key\":\"foo\",\"sample_rate\":0,\"sample_"
      "rand\":0.01006918276309107,\"release\":\"test-release\",\"environment\":"
      "\"production\",\"sampled\":\"false\"}}\n"
      "{\"type\":\"event\",\"length\":71}\n"
      "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\",\"some-"
      "context\":1234}\n"
      "{\"type\":\"minidump\",\"length\":4}\n"
      "MDMP\n"
      "{\"type\":\"attachment\",\"length\":12}\n"
      "Hello World!";

SENTRY_TEST(basic_http_request_preparation_for_event)
{
    SENTRY_TEST_DSN_NEW_DEFAULT(dsn);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry_value_t event = sentry_value_new_object();
    sentry_value_set_by_key(
        event, "event_id", sentry__value_new_uuid(&event_id));
    sentry__envelope_add_event(envelope, event);

    sentry_prepared_http_request_t *req
        = sentry__prepare_http_request(envelope, dsn, NULL, NULL);
    TEST_CHECK_STRING_EQUAL(req->method, "POST");
    TEST_CHECK_STRING_EQUAL(
        req->url, "https://sentry.invalid:443/api/42/envelope/");
#ifndef SENTRY_TRANSPORT_COMPRESSION
    TEST_CHECK_STRING_EQUAL(req->body,
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}\n"
        "{\"type\":\"event\",\"length\":51}\n"
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}");
#endif
    sentry__prepared_http_request_free(req);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(basic_http_request_preparation_for_transaction)
{
    SENTRY_TEST_DSN_NEW_DEFAULT(dsn);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry_value_t transaction = sentry_value_new_object();
    sentry_value_set_by_key(
        transaction, "event_id", sentry__value_new_uuid(&event_id));
    sentry_value_set_by_key(
        transaction, "type", sentry_value_new_string("transaction"));
    sentry__envelope_add_transaction(envelope, transaction);

    sentry_prepared_http_request_t *req
        = sentry__prepare_http_request(envelope, dsn, NULL, NULL);
    TEST_CHECK_STRING_EQUAL(req->method, "POST");
    TEST_CHECK_STRING_EQUAL(
        req->url, "https://sentry.invalid:443/api/42/envelope/");
#ifndef SENTRY_TRANSPORT_COMPRESSION
    TEST_CHECK_STRING_EQUAL(req->body,
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\","
        "\"sent_at\":"
        "\"2021-12-16T05:53:59.343Z\"}\n"
        "{\"type\":\"transaction\",\"length\":72}\n"
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\",\"type\":"
        "\"transaction\"}");
#endif
    sentry__prepared_http_request_free(req);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(basic_http_request_preparation_for_user_report)
{
    SENTRY_TEST_DSN_NEW_DEFAULT(dsn);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry_value_t user_report;
    SENTRY_TEST_DEPRECATED(
        user_report = sentry_value_new_user_feedback(
            &event_id, "some-name", "some-email", "some-comment"));
    sentry__envelope_add_user_report(envelope, user_report);

    sentry_prepared_http_request_t *req
        = sentry__prepare_http_request(envelope, dsn, NULL, NULL);
    TEST_CHECK_STRING_EQUAL(req->method, "POST");
    TEST_CHECK_STRING_EQUAL(
        req->url, "https://sentry.invalid:443/api/42/envelope/");
#ifndef SENTRY_TRANSPORT_COMPRESSION
    TEST_CHECK_STRING_EQUAL(req->body,
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}\n"
        "{\"type\":\"user_report\",\"length\":117}\n"
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\",\"name\":"
        "\"some-name\",\"email\":\"some-email\",\"comments\":"
        "\"some-comment\"}");
#endif
    sentry__prepared_http_request_free(req);
    sentry_value_decref(user_report);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(basic_http_request_preparation_for_user_feedback)
{
    SENTRY_TEST_DSN_NEW_DEFAULT(dsn);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry_value_t user_feedback = sentry_value_new_feedback(
        "some-message", "some-email", "some-name", &event_id);
    sentry__envelope_add_user_feedback(envelope, user_feedback);

    sentry_prepared_http_request_t *req
        = sentry__prepare_http_request(envelope, dsn, NULL, NULL);
    TEST_CHECK_STRING_EQUAL(req->method, "POST");
    TEST_CHECK_STRING_EQUAL(
        req->url, "https://sentry.invalid:443/api/42/envelope/");
#ifndef SENTRY_TRANSPORT_COMPRESSION
    char *line1 = req->body;
    char *line1_end = strchr(line1, '\n');
    TEST_CHECK(line1_end != NULL);
    line1_end[0] = '\0';
    TEST_CHECK_STRING_EQUAL(
        line1, "{\"event_id\":\"4c035723-8638-4c3a-923f-2ab9d08b4018\"}");

    char *line2 = line1_end + 1;
    char *line2_end = strchr(line2, '\n');
    TEST_CHECK(line2_end != NULL);
    line2_end[0] = '\0';
    TEST_CHECK_STRING_EQUAL(line2, "{\"type\":\"feedback\",\"length\":269}");

    char *line3 = line2_end + 1;
    sentry_value_t line3_json = sentry__value_from_json(line3, strlen(line3));
    TEST_CHECK(!sentry_value_is_null(line3_json));

    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(line3_json, "event_id")),
        "4c035723-8638-4c3a-923f-2ab9d08b4018");
    TEST_CHECK(!sentry_value_is_null(
        sentry_value_get_by_key(line3_json, "timestamp")));
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(line3_json, "platform")),
        "native");

    sentry_value_t contexts = sentry_value_get_by_key(line3_json, "contexts");
    TEST_CHECK(!sentry_value_is_null(contexts));

    sentry_value_t actual = sentry_value_get_by_key(contexts, "feedback");
    TEST_CHECK(!sentry_value_is_null(actual));

    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(actual, "message")),
        "some-message");
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                actual, "contact_email")),
        "some-email");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(actual, "name")),
        "some-name");
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                actual, "associated_event_id")),
        "c993afb6b4ac48a6b61b2558e601d65d");
    sentry_value_decref(line3_json);
#endif
    sentry__prepared_http_request_free(req);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(basic_http_request_preparation_for_event_with_attachment)
{
    SENTRY_TEST_DSN_NEW_DEFAULT(dsn);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry_value_t event = sentry_value_new_object();
    sentry_value_set_by_key(
        event, "event_id", sentry__value_new_uuid(&event_id));
    sentry__envelope_add_event(envelope, event);
    char msg[] = "Hello World!";
    sentry__envelope_add_from_buffer(
        envelope, msg, sizeof(msg) - 1, "attachment");

    sentry_prepared_http_request_t *req
        = sentry__prepare_http_request(envelope, dsn, NULL, NULL);
    TEST_CHECK_STRING_EQUAL(req->method, "POST");
    TEST_CHECK_STRING_EQUAL(
        req->url, "https://sentry.invalid:443/api/42/envelope/");
#ifndef SENTRY_TRANSPORT_COMPRESSION
    TEST_CHECK_STRING_EQUAL(req->body,
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}\n"
        "{\"type\":\"event\",\"length\":51}\n"
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}\n"
        "{\"type\":\"attachment\",\"length\":12}\n"
        "Hello World!");
#endif
    sentry__prepared_http_request_free(req);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(basic_http_request_preparation_for_minidump)
{
    SENTRY_TEST_DSN_NEW_DEFAULT(dsn);

    sentry_envelope_t *envelope = sentry__envelope_new();
    char dmp[] = "MDMP";
    sentry__envelope_add_from_buffer(
        envelope, dmp, sizeof(dmp) - 1, "minidump");
    char msg[] = "Hello World!";
    sentry__envelope_add_from_buffer(
        envelope, msg, sizeof(msg) - 1, "attachment");

    sentry_prepared_http_request_t *req
        = sentry__prepare_http_request(envelope, dsn, NULL, NULL);
    TEST_CHECK_STRING_EQUAL(req->method, "POST");
    TEST_CHECK_STRING_EQUAL(
        req->url, "https://sentry.invalid:443/api/42/envelope/");
#ifndef SENTRY_TRANSPORT_COMPRESSION
    TEST_CHECK_STRING_EQUAL(req->body,
        "{}\n"
        "{\"type\":\"minidump\",\"length\":4}\n"
        "MDMP\n"
        "{\"type\":\"attachment\",\"length\":12}\n"
        "Hello World!");
#endif
    sentry__prepared_http_request_free(req);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

sentry_envelope_t *
create_test_envelope()
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_release(options, "test-release");
    sentry_init(options);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry_value_t event = sentry_value_new_object();
    sentry_value_set_by_key(
        event, "event_id", sentry__value_new_uuid(&event_id));
    sentry_value_set_by_key(
        event, "some-context", sentry_value_new_int32(1234));
    sentry__envelope_add_event(envelope, event);

    char dmp[] = "MDMP";
    sentry__envelope_add_from_buffer(
        envelope, dmp, sizeof(dmp) - 1, "minidump");
    char msg[] = "Hello World!";
    sentry__envelope_add_from_buffer(
        envelope, msg, sizeof(msg) - 1, "attachment");
    return envelope;
}

SENTRY_TEST(serialize_envelope)
{
    sentry_envelope_t *envelope = create_test_envelope();

    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    sentry__envelope_serialize_into_stringbuilder(envelope, &sb);
    char *str = sentry__stringbuilder_into_string(&sb);
    TEST_ASSERT(!!str);

    TEST_CHECK_STRING_EQUAL(str, SERIALIZED_ENVELOPE_STR);

    sentry_envelope_free(envelope);
    sentry_free(str);

    sentry_close();
}

SENTRY_TEST(basic_write_envelope_to_file)
{
    sentry_envelope_t *envelope = create_test_envelope();
    const char *test_file_str = SENTRY_TEST_PATH_PREFIX "sentry_test_envelope";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);
    int rv = sentry_envelope_write_to_file(envelope, test_file_str);
    TEST_CHECK_INT_EQUAL(rv, 0);
    TEST_ASSERT(sentry__path_is_file(test_file_path));

    size_t test_file_size;
    char *test_file_content
        = sentry__path_read_to_buffer(test_file_path, &test_file_size);
    TEST_CHECK_INT_EQUAL(test_file_size, strlen(SERIALIZED_ENVELOPE_STR));
    TEST_CHECK_STRING_EQUAL(test_file_content, SERIALIZED_ENVELOPE_STR);

    sentry_free(test_file_content);
    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
    sentry_envelope_free(envelope);
    sentry_close();
}

SENTRY_TEST(read_write_envelope_to_file_null)
{
    sentry_envelope_t *empty_envelope = sentry__envelope_new();

    TEST_CHECK(!sentry_envelope_read_from_file(NULL));
    TEST_CHECK_INT_EQUAL(
        sentry_envelope_write_to_file(NULL, "irrelevant/path"), 1);
    TEST_CHECK_INT_EQUAL(
        sentry_envelope_write_to_file(empty_envelope, NULL), 1);
    TEST_CHECK_INT_EQUAL(
        sentry_envelope_write_to_file_n(NULL, "irrelevant/path", 0), 1);
    TEST_CHECK_INT_EQUAL(
        sentry_envelope_write_to_file_n(empty_envelope, NULL, 0), 1);

    sentry_envelope_free(empty_envelope);
}

SENTRY_TEST(read_write_envelope_to_invalid_path)
{
    sentry_envelope_t *envelope = create_test_envelope();
    const char *test_file_str = SENTRY_TEST_PATH_PREFIX
        "./directory_that_does_not_exist/sentry_test_envelope";
    TEST_CHECK(!sentry_envelope_read_from_file(test_file_str));
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);
    int rv = sentry_envelope_write_to_file(envelope, test_file_str);
    TEST_CHECK_INT_EQUAL(rv, 1);

    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
    sentry_envelope_free(envelope);
    sentry_close();
}

SENTRY_TEST(write_raw_envelope_to_file)
{
    sentry_envelope_t *envelope = create_test_envelope();
    const char *test_file_str = SENTRY_TEST_PATH_PREFIX "sentry_test_envelope";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);
    TEST_CHECK_INT_EQUAL(
        sentry_envelope_write_to_file(envelope, test_file_str), 0);

    sentry_envelope_t *raw_envelope
        = sentry__envelope_from_path(test_file_path);
    TEST_CHECK_INT_EQUAL(
        sentry_envelope_write_to_file(raw_envelope, test_file_str), 0);

    size_t test_file_size;
    char *test_file_content
        = sentry__path_read_to_buffer(test_file_path, &test_file_size);
    TEST_CHECK_INT_EQUAL(test_file_size, strlen(SERIALIZED_ENVELOPE_STR));
    TEST_CHECK_STRING_EQUAL(test_file_content, SERIALIZED_ENVELOPE_STR);
    TEST_CHECK_INT_EQUAL(sentry__path_remove(test_file_path), 0);

    sentry_free(test_file_content);
    sentry__path_free(test_file_path);
    sentry_envelope_free(envelope);
    sentry_envelope_free(raw_envelope);
    sentry_close();
}

SENTRY_TEST(raw_envelope_event_id)
{
    sentry_envelope_t *envelope = create_test_envelope();
    const char *test_file_str = SENTRY_TEST_PATH_PREFIX "sentry_test_envelope";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);
    TEST_CHECK_INT_EQUAL(
        sentry_envelope_write_to_file(envelope, test_file_str), 0);

    sentry_envelope_t *raw_envelope
        = sentry__envelope_from_path(test_file_path);
    TEST_CHECK(!!raw_envelope);

    sentry_uuid_t event_id = sentry__envelope_get_event_id(raw_envelope);
    char event_id_str[37];
    sentry_uuid_as_string(&event_id, event_id_str);
    TEST_CHECK_STRING_EQUAL(
        event_id_str, "c993afb6-b4ac-48a6-b61b-2558e601d65d");

    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
    sentry_envelope_free(envelope);
    sentry_envelope_free(raw_envelope);

    // missing event_id
    const char header_no_event_id[]
        = "{\"dsn\":\"https://foo@sentry.invalid/42\"}\n{}";
    test_file_path = sentry__path_from_str(test_file_str);
    TEST_CHECK_INT_EQUAL(
        sentry__path_write_buffer(
            test_file_path, header_no_event_id, sizeof(header_no_event_id) - 1),
        0);
    raw_envelope = sentry__envelope_from_path(test_file_path);
    TEST_CHECK(!!raw_envelope);
    event_id = sentry__envelope_get_event_id(raw_envelope);
    TEST_CHECK(sentry_uuid_is_nil(&event_id));
    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
    sentry_envelope_free(raw_envelope);

    // missing newline
    const char header_no_newline[]
        = "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}";
    test_file_path = sentry__path_from_str(test_file_str);
    TEST_CHECK_INT_EQUAL(sentry__path_write_buffer(test_file_path,
                             header_no_newline, sizeof(header_no_newline) - 1),
        0);
    raw_envelope = sentry__envelope_from_path(test_file_path);
    TEST_CHECK(!!raw_envelope);
    event_id = sentry__envelope_get_event_id(raw_envelope);
    sentry_uuid_as_string(&event_id, event_id_str);
    TEST_CHECK_STRING_EQUAL(
        event_id_str, "c993afb6-b4ac-48a6-b61b-2558e601d65d");
    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
    sentry_envelope_free(raw_envelope);

    sentry_close();
}

SENTRY_TEST(read_envelope_from_file)
{
    const char *test_file_str = SENTRY_TEST_PATH_PREFIX "sentry_test_envelope";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);
    TEST_CHECK_INT_EQUAL(
        sentry__path_write_buffer(test_file_path, SERIALIZED_ENVELOPE_STR,
            strlen(SERIALIZED_ENVELOPE_STR)),
        0);

    sentry_envelope_t *envelope = sentry_envelope_read_from_file(test_file_str);
    TEST_CHECK(!!envelope);

    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_envelope_get_header(envelope, "dsn")),
        "https://foo@sentry.invalid/42");
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_envelope_get_header(
                                envelope, "event_id")),
        "c993afb6-b4ac-48a6-b61b-2558e601d65d");

    sentry_uuid_t event_id = sentry__envelope_get_event_id(envelope);
    char event_id_str[37];
    sentry_uuid_as_string(&event_id, event_id_str);
    TEST_CHECK_STRING_EQUAL(
        event_id_str, "c993afb6-b4ac-48a6-b61b-2558e601d65d");

    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 3);

    sentry_value_t event = sentry_envelope_get_event(envelope);
    TEST_CHECK(!sentry_value_is_null(event));
    TEST_CHECK_INT_EQUAL(
        sentry_value_as_int32(sentry_value_get_by_key(event, "some-context")),
        1234);

    const sentry_envelope_item_t *item = sentry__envelope_get_item(envelope, 0);
    TEST_CHECK(!!item);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry__envelope_item_get_header(item, "type")),
        "event");
    size_t ev_len = 0;
    TEST_CHECK_STRING_EQUAL(sentry__envelope_item_get_payload(item, &ev_len),
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\","
        "\"some-context\":1234}");

    const sentry_envelope_item_t *minidump
        = sentry__envelope_get_item(envelope, 1);
    TEST_CHECK(!!minidump);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(
            sentry__envelope_item_get_header(minidump, "type")),
        "minidump");
    size_t dmp_len = 0;
    TEST_CHECK_STRING_EQUAL(
        sentry__envelope_item_get_payload(minidump, &dmp_len), "MDMP");
    TEST_CHECK_INT_EQUAL(dmp_len, 4);

    const sentry_envelope_item_t *attachment
        = sentry__envelope_get_item(envelope, 2);
    TEST_CHECK(!!attachment);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(
            sentry__envelope_item_get_header(attachment, "type")),
        "attachment");
    size_t attachment_len = 0;
    TEST_CHECK_STRING_EQUAL(
        sentry__envelope_item_get_payload(attachment, &attachment_len),
        "Hello World!");
    TEST_CHECK_INT_EQUAL(attachment_len, 12);

    sentry__path_free(test_file_path);
    sentry_envelope_free(envelope);
    sentry_close();
}

SENTRY_TEST(deserialize_envelope)
{
    const char *buf
        = "{\"event_id\":\"9ec79c33ec9942ab8353589fcb2e04dc\",\"dsn\":\"https:/"
          "/e12d836b15bb49d7bbf99e64295d995b:@sentry.io/42\"}\n"
          "{\"type\":\"attachment\",\"length\":10,\"content_type\":\"text/"
          "plain\",\"filename\":\"hello.txt\"}\n"
          "\xef\xbb\xbfHello\r\n\n"
          "{\"type\":\"event\",\"length\":41,\"content_type\":\"application/"
          "json\",\"filename\":\"application.log\"}\n"
          "{\"message\":\"hello world\",\"level\":\"error\"}\n";

    sentry_envelope_t *envelope = sentry_envelope_deserialize(buf, strlen(buf));
    TEST_CHECK(!!envelope);
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 2);

    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_envelope_get_header(envelope, "dsn")),
        "https://e12d836b15bb49d7bbf99e64295d995b:@sentry.io/42");
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_envelope_get_header(
                                envelope, "event_id")),
        "9ec79c33ec9942ab8353589fcb2e04dc");
    TEST_CHECK_JSON_VALUE(sentry_envelope_get_event(envelope),
        "{\"message\":\"hello world\",\"level\":\"error\"}");

    const sentry_envelope_item_t *attachment
        = sentry__envelope_get_item(envelope, 0);
    TEST_CHECK(!!attachment);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(
            sentry__envelope_item_get_header(attachment, "type")),
        "attachment");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(
            sentry__envelope_item_get_header(attachment, "filename")),
        "hello.txt");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(
            sentry__envelope_item_get_header(attachment, "content_type")),
        "text/plain");
    size_t attachment_len = 0;
    TEST_CHECK_STRING_EQUAL(
        sentry__envelope_item_get_payload(attachment, &attachment_len),
        "\xef\xbb\xbfHello\r\n");
    TEST_CHECK_INT_EQUAL(attachment_len, 10);

    sentry_envelope_free(envelope);
}

static void
test_deserialize_envelope_empty_attachments(const char *buf, size_t buf_len)
{
    sentry_envelope_t *envelope = sentry_envelope_deserialize(buf, buf_len);
    TEST_CHECK(!!envelope);
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 2);

    TEST_CHECK(
        sentry_value_is_null(sentry_envelope_get_header(envelope, "dsn")));
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_envelope_get_header(
                                envelope, "event_id")),
        "9ec79c33ec9942ab8353589fcb2e04dc");
    TEST_CHECK(sentry_value_is_null(sentry_envelope_get_event(envelope)));

    for (int i = 0; i < 2; ++i) {
        const sentry_envelope_item_t *attachment
            = sentry__envelope_get_item(envelope, i);
        TEST_CHECK(!!attachment);
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(
                sentry__envelope_item_get_header(attachment, "type")),
            "attachment");
        TEST_CHECK_INT_EQUAL(
            sentry_value_as_int32(
                sentry__envelope_item_get_header(attachment, "length")),
            0);
        size_t attachment_len = 0;
        TEST_CHECK(
            !sentry__envelope_item_get_payload(attachment, &attachment_len));
        TEST_CHECK_INT_EQUAL(attachment_len, 0);
    }

    sentry_envelope_free(envelope);
}

SENTRY_TEST(deserialize_envelope_empty_attachments)
{
    const char *buf = "{\"event_id\":\"9ec79c33ec9942ab8353589fcb2e04dc\"}\n"
                      "{\"type\":\"attachment\",\"length\":0}\n"
                      "\n"
                      "{\"type\":\"attachment\",\"length\":0}\n"
                      "\n";
    size_t buf_len = strlen(buf);

    // trailing newline
    test_deserialize_envelope_empty_attachments(buf, buf_len);

    // eof
    test_deserialize_envelope_empty_attachments(buf, buf_len - 1);
}

static void
test_deserialize_envelope_implicit_length(const char *buf, size_t buf_len)
{
    sentry_envelope_t *envelope = sentry_envelope_deserialize(buf, buf_len);
    TEST_CHECK(!!envelope);
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 1);

    TEST_CHECK(
        sentry_value_is_null(sentry_envelope_get_header(envelope, "dsn")));
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_envelope_get_header(
                                envelope, "event_id")),
        "9ec79c33ec9942ab8353589fcb2e04dc");
    TEST_CHECK(sentry_value_is_null(sentry_envelope_get_event(envelope)));

    const sentry_envelope_item_t *attachment
        = sentry__envelope_get_item(envelope, 0);
    TEST_CHECK(!!attachment);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(
            sentry__envelope_item_get_header(attachment, "type")),
        "attachment");
    TEST_CHECK_INT_EQUAL(sentry_value_as_int32(sentry__envelope_item_get_header(
                             attachment, "length")),
        0);
    size_t attachment1_len = 0;
    TEST_CHECK_STRING_EQUAL(
        sentry__envelope_item_get_payload(attachment, &attachment1_len),
        "helloworld");
    TEST_CHECK_INT_EQUAL(attachment1_len, 10);

    sentry_envelope_free(envelope);
}

SENTRY_TEST(deserialize_envelope_implicit_length)
{
    const char *buf = "{\"event_id\":\"9ec79c33ec9942ab8353589fcb2e04dc\"}\n"
                      "{\"type\":\"attachment\"}\n"
                      "helloworld\n";
    size_t buf_len = strlen(buf);

    // trailing newline
    test_deserialize_envelope_implicit_length(buf, buf_len);

    // eof
    test_deserialize_envelope_implicit_length(buf, buf_len - 1);
}

SENTRY_TEST(deserialize_envelope_no_headers)
{
    const char *session = "{\"started\": "
                          "\"2020-02-07T14:16:00Z\",\"attrs\":{\"release\":"
                          "\"sentry-test@1.0.0\"}"
                          "}";
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{}\n"
        "{\"type\":\"session\"}\n"
        "%s",
        session);

    sentry_envelope_t *envelope = sentry_envelope_deserialize(buf, strlen(buf));
    TEST_CHECK(!!envelope);
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 1);

    TEST_CHECK(
        sentry_value_is_null(sentry_envelope_get_header(envelope, "dsn")));
    TEST_CHECK(
        sentry_value_is_null(sentry_envelope_get_header(envelope, "event_id")));
    TEST_CHECK(sentry_value_is_null(sentry_envelope_get_event(envelope)));

    const sentry_envelope_item_t *item = sentry__envelope_get_item(envelope, 0);
    TEST_CHECK(!!item);
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry__envelope_item_get_header(item, "type")),
        "session");
    TEST_CHECK(
        sentry_value_is_null(sentry__envelope_item_get_header(item, "length")));
    size_t session_len = 0;
    TEST_CHECK_STRING_EQUAL(
        sentry__envelope_item_get_payload(item, &session_len), session);
    TEST_CHECK_INT_EQUAL(session_len, strlen(session));

    sentry_envelope_free(envelope);
}

static void
test_deserialize_envelope_empty(const char *buf, size_t buf_len)
{
    sentry_envelope_t *envelope = sentry_envelope_deserialize(buf, buf_len);
    TEST_CHECK(!!envelope);
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 0);

    TEST_CHECK(
        sentry_value_is_null(sentry_envelope_get_header(envelope, "dsn")));
    TEST_CHECK(
        sentry_value_is_null(sentry_envelope_get_header(envelope, "event_id")));
    TEST_CHECK(sentry_value_is_null(sentry_envelope_get_event(envelope)));

    sentry_envelope_free(envelope);
}

SENTRY_TEST(deserialize_envelope_empty)
{
    const char *buf = "{}\n";
    size_t buf_len = strlen(buf);

    // trailing newline
    test_deserialize_envelope_empty(buf, buf_len);

    // eof
    test_deserialize_envelope_empty(buf, buf_len - 1);
}

SENTRY_TEST(envelope_materialize)
{
    TEST_CHECK(!sentry__envelope_materialize(NULL));

    const char *path_str
        = SENTRY_TEST_PATH_PREFIX "sentry_test_envelope_materialize";
    sentry_path_t *path = sentry__path_from_str(path_str);

    sentry_envelope_t *src = sentry__envelope_new();
    sentry__envelope_add_event(src, sentry_value_new_object());
    TEST_ASSERT(sentry_envelope_write_to_path(src, path) == 0);
    sentry_envelope_free(src);

    sentry_envelope_t *raw = sentry__envelope_from_path(path);
    TEST_ASSERT(!!raw);
    TEST_CHECK(sentry__envelope_materialize(raw));
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(raw), 1);
    TEST_CHECK(sentry__envelope_materialize(raw));
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(raw), 1);
    sentry_envelope_free(raw);

    TEST_ASSERT(sentry__path_write_buffer(path, "garbage", 7) == 0);
    sentry_envelope_t *bad = sentry__envelope_from_path(path);
    TEST_ASSERT(!!bad);
    TEST_CHECK(!sentry__envelope_materialize(bad));
    TEST_CHECK(sentry__envelope_is_raw(bad));
    size_t serialized_len = 0;
    char *serialized = sentry_envelope_serialize(bad, &serialized_len);
    TEST_ASSERT(!!serialized);
    TEST_CHECK_INT_EQUAL(serialized_len, 7);
    TEST_CHECK_STRING_EQUAL(serialized, "garbage");
    sentry_free(serialized);
    sentry_envelope_free(bad);

    sentry__path_remove(path);
    sentry__path_free(path);
}

SENTRY_TEST(envelope_remove_item)
{
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry_envelope_item_t *first
        = sentry__envelope_add_from_buffer(envelope, "one", 3, "attachment");
    sentry_envelope_item_t *second
        = sentry__envelope_add_from_buffer(envelope, "two", 3, "attachment");

    TEST_ASSERT(!!first);
    TEST_ASSERT(!!second);
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 2);

    TEST_CHECK(sentry__envelope_remove_item(envelope, first));
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 1);
    TEST_CHECK(sentry__envelope_get_item(envelope, 0) == second);

    size_t payload_len = 0;
    TEST_CHECK_STRING_EQUAL(
        sentry__envelope_item_get_payload(second, &payload_len), "two");
    TEST_CHECK_INT_EQUAL(payload_len, 3);

    TEST_CHECK(sentry__envelope_remove_item(envelope, second));
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 0);
    TEST_CHECK(!sentry__envelope_get_item(envelope, 0));

    sentry_envelope_free(envelope);
}

SENTRY_TEST(attachment_ref_creation)
{
    const char *test_file_str
        = SENTRY_TEST_PATH_PREFIX "sentry_test_attachment_ref";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);

    // Small file: should be added to envelope
    {
        sentry_envelope_t *envelope = sentry__envelope_new();
        char small_data[] = "small";
        TEST_CHECK_INT_EQUAL(sentry__path_write_buffer(test_file_path,
                                 small_data, sizeof(small_data) - 1),
            0);

        sentry_attachment_t *attachment
            = sentry__attachment_from_path(sentry__path_clone(test_file_path));
        sentry_envelope_item_t *item
            = sentry__envelope_add_attachment(envelope, attachment);

        TEST_CHECK(!!item);
        TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 1);
        size_t payload_len = 0;
        TEST_CHECK_STRING_EQUAL(
            sentry__envelope_item_get_payload(item, &payload_len), "small");

        sentry__attachment_free(attachment);
        sentry_envelope_free(envelope);
    }

    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
}

SENTRY_TEST(attachment_ref_from_path)
{
    const char *test_file_str
        = SENTRY_TEST_PATH_PREFIX "sentry_test_attachment_ref_from_path";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);

    // Small file: should be added to envelope
    {
        sentry_envelope_t *envelope = sentry__envelope_new();
        char small_data[] = "small";
        TEST_CHECK_INT_EQUAL(sentry__path_write_buffer(test_file_path,
                                 small_data, sizeof(small_data) - 1),
            0);

        sentry_envelope_item_t *item = sentry__envelope_add_from_path(
            envelope, test_file_path, "attachment");

        TEST_CHECK(!!item);
        TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 1);
        size_t payload_len = 0;
        TEST_CHECK_STRING_EQUAL(
            sentry__envelope_item_get_payload(item, &payload_len), "small");

        sentry_envelope_free(envelope);
    }

    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
}

static void
check_attachment_ref_item(const sentry_envelope_t *envelope, size_t item_idx,
    const char *expected_filename, const char *expected_content_type,
    const char *expected_attachment_type, uint64_t expected_length,
    const char *expected_path)
{
    const sentry_envelope_item_t *item
        = sentry__envelope_get_item(envelope, item_idx);
    TEST_ASSERT(!!item);

    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(
            sentry__envelope_item_get_header(item, "content_type")),
        "application/vnd.sentry.attachment-ref+json");
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(
            sentry__envelope_item_get_header(item, "filename")),
        expected_filename);
    if (expected_attachment_type) {
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(
                sentry__envelope_item_get_header(item, "attachment_type")),
            expected_attachment_type);
    }
    sentry_value_t len_val
        = sentry__envelope_item_get_header(item, "attachment_length");
    uint64_t actual_len = (uint64_t)sentry_value_as_uint64(len_val);
    if (actual_len == 0) {
        int64_t as_i64 = sentry_value_as_int64(len_val);
        if (as_i64 == 0) {
            actual_len = (uint64_t)sentry_value_as_int32(len_val);
        } else {
            actual_len = (uint64_t)as_i64;
        }
    }
    TEST_CHECK(actual_len == expected_length);

    sentry_attachment_ref_t ref;
    TEST_CHECK(sentry__envelope_item_get_attachment_ref(item, &ref));
    TEST_CHECK_STRING_EQUAL(ref.path, expected_path);
    if (expected_content_type) {
        TEST_CHECK_STRING_EQUAL(ref.content_type, expected_content_type);
    }
    sentry__attachment_ref_cleanup(&ref);
}

SENTRY_TEST(attachment_ref_copy)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_init(options);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");

    const char *test_file_str
        = SENTRY_TEST_PATH_PREFIX "sentry_test_minidump.dmp";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);
    FILE *f = fopen(test_file_str, "wb");
    TEST_CHECK(!!f);
    fputs("minidump_data", f);
    fclose(f);

    sentry_attachment_t *attachment
        = sentry__attachment_from_path(sentry__path_clone(test_file_path));
    sentry_attachment_set_type(attachment, SENTRY_ATTACHMENT_TYPE_MINIDUMP);
    sentry_attachment_set_content_type(attachment, "application/x-dmp");

    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry__envelope_set_event_id(envelope, &event_id);
    sentry_path_t *db_path = NULL;
    SENTRY_WITH_OPTIONS (opts) {
        db_path = sentry__path_clone(opts->database_path);
        // no run_path passed → copy, not move
        sentry__cache_attachment_ref(
            envelope, attachment, opts->run->cache_path, NULL);
    }

    // original file still exists (copied, not moved)
    TEST_CHECK(sentry__path_is_file(test_file_path));

    // cached minidumps keep the legacy <uuid>.dmp name
    sentry_path_t *cache_dir = sentry__path_join_str(db_path, "cache");
    sentry_path_t *cached = sentry__path_join_str(
        cache_dir, "c993afb6-b4ac-48a6-b61b-2558e601d65d.dmp");
    TEST_CHECK(sentry__path_is_file(cached));

    // envelope carries an attachment-ref item with the expected headers
    TEST_ASSERT(sentry__envelope_get_item_count(envelope) == 1);
    check_attachment_ref_item(envelope, 0, "sentry_test_minidump.dmp",
        "application/x-dmp", SENTRY_ATTACHMENT_TYPE_MINIDUMP,
        strlen("minidump_data"), "c993afb6-b4ac-48a6-b61b-2558e601d65d.dmp");

    sentry_path_t *cached_envelope = sentry__path_join_str(
        cache_dir, "c993afb6-b4ac-48a6-b61b-2558e601d65d.envelope");
    sentry__path_remove(cached_envelope);

    TEST_CHECK_INT_EQUAL(
        sentry__envelope_write_to_cache(envelope, cache_dir), 0);
    size_t cached_len = 0;
    char *cached_payload = sentry__path_read_to_buffer(cached, &cached_len);
    TEST_ASSERT(!!cached_payload);
    TEST_CHECK_INT_EQUAL(cached_len, strlen("minidump_data"));
    TEST_CHECK(memcmp(cached_payload, "minidump_data", cached_len) == 0);
    sentry_free(cached_payload);

    sentry_envelope_free(envelope);
    sentry__path_remove(cached_envelope);
    sentry__path_free(cached_envelope);
    sentry__path_remove(cached);
    sentry__path_free(cache_dir);
    sentry__path_free(cached);
    sentry__path_free(db_path);
    sentry__attachment_free(attachment);
    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
    sentry_close();
}

SENTRY_TEST(attachment_ref_move)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_init(options);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");

    // create an attachment file inside the run directory (SDK-owned)
    sentry_path_t *run_path = NULL;
    sentry_path_t *db_path = NULL;
    SENTRY_WITH_OPTIONS (opts) {
        run_path = sentry__path_clone(opts->run->run_path);
        db_path = sentry__path_clone(opts->database_path);
    }
    TEST_CHECK(!!run_path);
    sentry_path_t *src_path
        = sentry__path_join_str(run_path, "test_attachment.bin");

#ifdef SENTRY_PLATFORM_WINDOWS
    FILE *f = _wfopen(src_path->path_w, L"wb");
#else
    FILE *f = fopen(src_path->path, "wb");
#endif
    TEST_CHECK(!!f);
    fputs("attachment_data", f);
    fclose(f);

    sentry_attachment_t *attachment
        = sentry__attachment_from_path(sentry__path_clone(src_path));

    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry__envelope_set_event_id(envelope, &event_id);
    SENTRY_WITH_OPTIONS (opts) {
        // run_path passed → file is renamed (moved)
        sentry__cache_attachment_ref(
            envelope, attachment, opts->run->cache_path, run_path);
    }

    // run-dir file is gone (renamed)
    TEST_CHECK(!sentry__path_is_file(src_path));

    // cached file exists in cache dir as <uuid>-<filename>
    sentry_path_t *cache_dir = sentry__path_join_str(db_path, "cache");
    sentry_path_t *cached = sentry__path_join_str(
        cache_dir, "c993afb6-b4ac-48a6-b61b-2558e601d65d-test_attachment.bin");
    sentry__path_free(cache_dir);
    TEST_CHECK(sentry__path_is_file(cached));

    // envelope carries an attachment-ref item
    TEST_ASSERT(sentry__envelope_get_item_count(envelope) == 1);
    check_attachment_ref_item(envelope, 0, "test_attachment.bin", NULL, NULL,
        strlen("attachment_data"),
        "c993afb6-b4ac-48a6-b61b-2558e601d65d-test_attachment.bin");

    sentry_envelope_free(envelope);
    sentry__path_remove(cached);
    sentry__path_free(cached);
    sentry__path_free(db_path);
    sentry__attachment_free(attachment);
    sentry__path_free(src_path);
    sentry__path_free(run_path);
    sentry_close();
}

SENTRY_TEST(attachment_ref_cache_cleanup)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    const char raw_data[]
        = "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}\n";
    sentry_path_t *raw_path
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX "sentry_test_raw_ref");
    TEST_CHECK_INT_EQUAL(
        sentry__path_write_buffer(raw_path, raw_data, sizeof(raw_data) - 1), 0);
    sentry_envelope_t *raw_envelope = sentry__envelope_from_path(raw_path);
    TEST_ASSERT(!!raw_envelope);

    sentry_attachment_t *attachment = sentry__attachment_from_buffer(
        "payload", strlen("payload"), sentry__path_from_str("payload.bin"));
    sentry_path_t *cache_path = NULL;
    SENTRY_WITH_OPTIONS (opts) {
        cache_path = sentry__path_clone(opts->run->cache_path);
        TEST_CHECK(!sentry__cache_attachment_ref(
            raw_envelope, attachment, opts->run->cache_path, NULL));
    }

    sentry_path_t *cached = sentry__path_join_str(
        cache_path, "c993afb6-b4ac-48a6-b61b-2558e601d65d-payload.bin");
    TEST_CHECK(!sentry__path_is_file(cached));

    sentry__path_free(cached);
    sentry__path_free(cache_path);
    sentry__attachment_free(attachment);
    sentry_envelope_free(raw_envelope);
    sentry__path_remove(raw_path);
    sentry__path_free(raw_path);
    sentry_close();
}

SENTRY_TEST(attachment_ref_cache_discard)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_enable_large_attachments(options, true);
    sentry__client_report_reset();

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry__envelope_set_event_id(envelope, &event_id);

    sentry_path_t *cache_path = sentry__path_from_str(
        SENTRY_TEST_PATH_PREFIX "sentry_test_attachment_ref_cache_file");
    TEST_CHECK_INT_EQUAL(sentry__path_write_buffer(cache_path, "x", 1), 0);

    sentry_attachment_t *attachment = sentry__attachment_from_buffer(
        "x", 1, sentry__path_from_str("large.bin"));
    attachment->buf_len = SENTRY_LARGE_ATTACHMENT_SIZE;

    sentry__cache_attachment_refs(
        envelope, attachment, options, cache_path, NULL);
    TEST_CHECK_INT_EQUAL(sentry__envelope_get_item_count(envelope), 0);

    sentry_client_report_t report = { { 0 } };
    TEST_CHECK(sentry__client_report_save(&report));
    TEST_CHECK_INT_EQUAL(report.counts[SENTRY_DISCARD_REASON_SEND_ERROR]
                                      [SENTRY_DATA_CATEGORY_ATTACHMENT],
        1);

    sentry__attachment_free(attachment);
    sentry__path_remove(cache_path);
    sentry__path_free(cache_path);
    sentry_envelope_free(envelope);
    sentry_options_free(options);
    sentry__client_report_reset();
}

SENTRY_TEST(attachment_ref_roundtrip)
{
    // An envelope with an attachment-ref item carrying `path` survives
    // a serialize / deserialize round-trip and preserves all headers + path.
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry_value_t event = sentry_value_new_object();
    sentry__envelope_add_event(envelope, event);

    sentry_attachment_ref_t ref = { 0 };
    ref.path = "abc-crashlog.bin";
    ref.content_type = "application/octet-stream";
    sentry__envelope_add_attachment_ref(
        envelope, &ref, "crashlog.bin", NULL, 12345);

    size_t buf_len = 0;
    char *buf = sentry_envelope_serialize(envelope, &buf_len);
    TEST_ASSERT(!!buf);
    sentry_envelope_free(envelope);

    sentry_envelope_t *parsed = sentry_envelope_deserialize(buf, buf_len);
    sentry_free(buf);
    TEST_ASSERT(!!parsed);
    TEST_ASSERT(sentry__envelope_get_item_count(parsed) == 2);

    check_attachment_ref_item(parsed, 1, "crashlog.bin",
        "application/octet-stream", NULL, 12345, "abc-crashlog.bin");

    sentry_envelope_free(parsed);
}

SENTRY_TEST(deserialize_envelope_invalid)
{
    TEST_CHECK(!sentry_envelope_deserialize("", 0));
    TEST_CHECK(!sentry_envelope_deserialize("{}", 0));
    TEST_CHECK(!sentry_envelope_deserialize("\n", 1));
    TEST_CHECK(!sentry_envelope_deserialize("{}\n{}", 5));
    TEST_CHECK(!sentry_envelope_deserialize("{}\ninvalid\n", 11));
    TEST_CHECK(!sentry_envelope_deserialize("invalid", 7));
    TEST_CHECK(!sentry_envelope_deserialize("{}\n{\"length\":-1}\n", 17));
    char buf[128];
    snprintf(buf, sizeof(buf), "{}\n{\"length\":%zu}\n", SIZE_MAX);
    TEST_CHECK(!sentry_envelope_deserialize(buf, strlen(buf)));
}

SENTRY_TEST(envelope_can_add_client_report)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry__envelope_add_from_buffer(envelope, "{}", 2, "event");
    TEST_CHECK(sentry__envelope_can_add_client_report(envelope, NULL));

    sentry_rate_limiter_t *rl = sentry__rate_limiter_new();
    TEST_CHECK(sentry__envelope_can_add_client_report(envelope, rl));

    sentry__rate_limiter_update_from_header(rl, "60:error:organization");
    TEST_CHECK(!sentry__envelope_can_add_client_report(envelope, rl));

    sentry_envelope_free(envelope);

    // Empty envelope
    envelope = sentry__envelope_new();
    TEST_CHECK(!sentry__envelope_can_add_client_report(envelope, NULL));

    sentry_envelope_free(envelope);

    // Envelope with only internal items
    envelope = sentry__envelope_new();
    sentry__envelope_add_from_buffer(envelope, "{}", 2, "client_report");
    TEST_CHECK(!sentry__envelope_can_add_client_report(envelope, NULL));

    sentry_envelope_free(envelope);

    // Raw envelope
    envelope = sentry__envelope_new();
    sentry__envelope_add_from_buffer(envelope, "{}", 2, "event");
    const char *path_str = SENTRY_TEST_PATH_PREFIX "sentry_test_raw";
    sentry_path_t *path = sentry__path_from_str(path_str);
    TEST_CHECK_INT_EQUAL(sentry_envelope_write_to_path(envelope, path), 0);
    sentry_envelope_free(envelope);

    sentry_envelope_t *raw = sentry__envelope_from_path(path);
    TEST_CHECK(!!raw);
    TEST_CHECK(!sentry__envelope_can_add_client_report(raw, NULL));

    sentry_envelope_free(raw);
    sentry__path_remove(path);
    sentry__path_free(path);
    sentry__rate_limiter_free(rl);
    sentry_close();
}

#if defined(SENTRY_PLATFORM_UNIX) && !defined(SENTRY_PLATFORM_ANDROID)         \
    && !defined(SENTRY_PLATFORM_PS) && !defined(SENTRY_PLATFORM_NX)
static void
sigxfsz_noop(int sig)
{
    (void)sig;
}
#endif

SENTRY_TEST(write_envelope_partial_write_fails)
{
#if !defined(SENTRY_PLATFORM_UNIX) || defined(SENTRY_PLATFORM_ANDROID)         \
    || defined(SENTRY_PLATFORM_PS) || defined(SENTRY_PLATFORM_NX)
    SKIP_TEST();
#else
    sentry_options_t *options = sentry_options_new();
    TEST_CHECK(options != NULL);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_backend(options, NULL);
    sentry_init(options);

    sentry_envelope_t *envelope = sentry__envelope_new();
    TEST_CHECK(envelope != NULL);

    size_t big_len = 40000;
    char *big_buf = sentry_malloc(big_len);
    TEST_CHECK(big_buf != NULL);
    memset(big_buf, 'X', big_len);
    sentry__envelope_add_from_buffer(envelope, big_buf, big_len, "attachment");
    sentry_free(big_buf);

    const char *test_file_str
        = SENTRY_TEST_PATH_PREFIX "sentry_test_partial_write";
    sentry_path_t *test_path = sentry__path_from_str(test_file_str);
    TEST_CHECK(test_path != NULL);
    sentry__path_remove(test_path);

    // fork() isolates the RLIMIT_FSIZE setting from the test suite
    pid_t pid = fork();
    TEST_CHECK(pid >= 0);

    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigxfsz_noop;
        sigaction(SIGXFSZ, &sa, NULL);

        struct rlimit rl;
        rl.rlim_cur = 512;
        rl.rlim_max = 512;
        if (setrlimit(RLIMIT_FSIZE, &rl) != 0) {
            _exit(2);
        }

        int rv = sentry_envelope_write_to_file(envelope, test_file_str);
        sentry_envelope_free(envelope);
        sentry__path_free(test_path);
        _exit(rv != 0 ? 0 : 1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    TEST_CHECK(WIFEXITED(status));
    int child_exit = WEXITSTATUS(status);
    TEST_CHECK_INT_EQUAL(child_exit, 0);

    TEST_CHECK(!sentry__path_is_file(test_path));
    sentry__path_remove(test_path);
    sentry__path_free(test_path);

    sentry_envelope_free(envelope);
    sentry_close();
#endif
}
