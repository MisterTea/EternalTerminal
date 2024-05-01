#include "sentry_envelope.h"
#include "sentry_path.h"
#include "sentry_testsupport.h"
#include "sentry_transport.h"
#include "sentry_utils.h"
#include "sentry_value.h"

static char *const SERIALIZED_ENVELOPE_STR
    = "{\"dsn\":\"https://foo@sentry.invalid/42\","
      "\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}\n"
      "{\"type\":\"event\",\"length\":71}\n"
      "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\",\"some-"
      "context\":null}\n"
      "{\"type\":\"minidump\",\"length\":4}\n"
      "MDMP\n"
      "{\"type\":\"attachment\",\"length\":12}\n"
      "Hello World!";

SENTRY_TEST(basic_http_request_preparation_for_event)
{
    sentry_dsn_t *dsn = sentry__dsn_new("https://foo@sentry.invalid/42");

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
    TEST_CHECK_STRING_EQUAL(req->body,
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}\n"
        "{\"type\":\"event\",\"length\":51}\n"
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}");
    sentry__prepared_http_request_free(req);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(basic_http_request_preparation_for_transaction)
{
    sentry_dsn_t *dsn = sentry__dsn_new("https://foo@sentry.invalid/42");

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
    TEST_CHECK_STRING_EQUAL(req->body,
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\",\"sent_at\":"
        "\"2021-12-16T05:53:59.343Z\"}\n"
        "{\"type\":\"transaction\",\"length\":72}\n"
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\",\"type\":"
        "\"transaction\"}");

    sentry__prepared_http_request_free(req);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(basic_http_request_preparation_for_event_with_attachment)
{
    sentry_dsn_t *dsn = sentry__dsn_new("https://foo@sentry.invalid/42");

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
    TEST_CHECK_STRING_EQUAL(req->body,
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}\n"
        "{\"type\":\"event\",\"length\":51}\n"
        "{\"event_id\":\"c993afb6-b4ac-48a6-b61b-2558e601d65d\"}\n"
        "{\"type\":\"attachment\",\"length\":12}\n"
        "Hello World!");
    sentry__prepared_http_request_free(req);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

SENTRY_TEST(basic_http_request_preparation_for_minidump)
{
    sentry_dsn_t *dsn = sentry__dsn_new("https://foo@sentry.invalid/42");

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
    TEST_CHECK_STRING_EQUAL(req->body,
        "{}\n"
        "{\"type\":\"minidump\",\"length\":4}\n"
        "MDMP\n"
        "{\"type\":\"attachment\",\"length\":12}\n"
        "Hello World!");
    sentry__prepared_http_request_free(req);
    sentry_envelope_free(envelope);

    sentry__dsn_decref(dsn);
}

sentry_envelope_t *
create_test_envelope()
{
    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_init(options);

    sentry_uuid_t event_id
        = sentry_uuid_from_string("c993afb6-b4ac-48a6-b61b-2558e601d65d");
    sentry_envelope_t *envelope = sentry__envelope_new();
    sentry_value_t event = sentry_value_new_object();
    sentry_value_set_by_key(
        event, "event_id", sentry__value_new_uuid(&event_id));
    sentry_value_set_by_key(event, "some-context", sentry_value_new_null());
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

    TEST_CHECK_STRING_EQUAL(str, SERIALIZED_ENVELOPE_STR);

    sentry_envelope_free(envelope);
    sentry_free(str);

    sentry_close();
}

SENTRY_TEST(basic_write_envelope_to_file)
{
    sentry_envelope_t *envelope = create_test_envelope();
    const char *test_file_str = "sentry_test_envelope";
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

SENTRY_TEST(write_envelope_to_file_null)
{
    sentry_envelope_t *empty_envelope = sentry__envelope_new();

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
