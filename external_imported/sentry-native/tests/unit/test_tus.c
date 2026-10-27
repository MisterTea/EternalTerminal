#include "sentry_client_report.h"
#include "sentry_envelope.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_testsupport.h"
#include "sentry_utils.h"
#include "transports/sentry_http_transport.h"

#if !defined(SENTRY_PLATFORM_ANDROID) && !defined(SENTRY_PLATFORM_NX)          \
    && !defined(SENTRY_PLATFORM_PS) && !defined(SENTRY_PLATFORM_XBOX)
static void
create_large_test_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(!!f);
    TEST_ASSERT_INT_EQUAL(
        fseek(f, (long)(SENTRY_LARGE_ATTACHMENT_SIZE - 1), SEEK_SET), 0);
    TEST_ASSERT(fputc(0, f) != EOF);
    TEST_ASSERT_INT_EQUAL(fclose(f), 0);
}
#endif

SENTRY_TEST(tus_upload_url)
{
    SENTRY_TEST_DSN_NEW_DEFAULT(dsn);

    char *url = sentry__dsn_get_upload_url(dsn);
    TEST_CHECK_STRING_EQUAL(url, "https://sentry.invalid:443/api/42/upload/");
    sentry_free(url);
    sentry__dsn_decref(dsn);

    TEST_CHECK(!sentry__dsn_get_upload_url(NULL));
}

SENTRY_TEST(tus_request_preparation)
{
    SENTRY_TEST_DSN_NEW_DEFAULT(dsn);

    // Test creation request (POST, no body) without attachment_type
    sentry_prepared_http_request_t *req
        = sentry__prepare_tus_create_request(9, NULL, dsn, NULL);
    TEST_CHECK(!!req);
    TEST_CHECK_STRING_EQUAL(req->method, "POST");
    TEST_CHECK_STRING_EQUAL(
        req->url, "https://sentry.invalid:443/api/42/upload/");
    TEST_CHECK(!req->body_path);
    TEST_CHECK_INT_EQUAL(req->body_len, 0);
    TEST_CHECK(!req->body);

    bool has_tus_resumable = false;
    bool has_upload_length = false;
    bool has_content_type = false;
    bool has_upload_metadata = false;
    for (size_t i = 0; i < req->headers_len; i++) {
        if (strcmp(req->headers[i].key, "tus-resumable") == 0) {
            TEST_CHECK_STRING_EQUAL(req->headers[i].value, "1.0.0");
            has_tus_resumable = true;
        }
        if (strcmp(req->headers[i].key, "upload-length") == 0) {
            TEST_CHECK_STRING_EQUAL(req->headers[i].value, "9");
            has_upload_length = true;
        }
        if (strcmp(req->headers[i].key, "content-type") == 0) {
            has_content_type = true;
        }
        if (strcmp(req->headers[i].key, "upload-metadata") == 0) {
            has_upload_metadata = true;
        }
    }
    TEST_CHECK(has_tus_resumable);
    TEST_CHECK(has_upload_length);
    TEST_CHECK(!has_content_type);
    TEST_CHECK(!has_upload_metadata);

    sentry__prepared_http_request_free(req);

    // Test creation request with attachment_type
    req = sentry__prepare_tus_create_request(9, "event.minidump", dsn, NULL);
    TEST_CHECK(!!req);
    has_upload_metadata = false;
    for (size_t i = 0; i < req->headers_len; i++) {
        if (strcmp(req->headers[i].key, "upload-metadata") == 0) {
            has_upload_metadata = true;
            TEST_CHECK(strncmp(req->headers[i].value, "sentry ", 7) == 0);
            TEST_CHECK(strlen(req->headers[i].value) > 7);
        }
    }
    TEST_CHECK(has_upload_metadata);
    sentry__prepared_http_request_free(req);

    // Test upload request (PATCH with body)
    const char *test_file_str = SENTRY_TEST_PATH_PREFIX "sentry_test_tus_file";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);
    TEST_CHECK_INT_EQUAL(
        sentry__path_write_buffer(test_file_path, "test-data", 9), 0);

    const char *location = "https://sentry.invalid/api/42/upload/abc123/";
    req = sentry__prepare_tus_upload_request(
        location, test_file_path, 9, dsn, NULL);
    TEST_CHECK(!!req);
    TEST_CHECK_STRING_EQUAL(req->method, "PATCH");
    TEST_CHECK_STRING_EQUAL(req->url, location);
    TEST_CHECK(!!req->body_path);
    TEST_CHECK_INT_EQUAL(req->body_len, 9);
    TEST_CHECK(!req->body);

    has_tus_resumable = false;
    has_content_type = false;
    bool has_upload_offset = false;
    for (size_t i = 0; i < req->headers_len; i++) {
        if (strcmp(req->headers[i].key, "tus-resumable") == 0) {
            TEST_CHECK_STRING_EQUAL(req->headers[i].value, "1.0.0");
            has_tus_resumable = true;
        }
        if (strcmp(req->headers[i].key, "content-type") == 0) {
            TEST_CHECK_STRING_EQUAL(
                req->headers[i].value, "application/offset+octet-stream");
            has_content_type = true;
        }
        if (strcmp(req->headers[i].key, "upload-offset") == 0) {
            TEST_CHECK_STRING_EQUAL(req->headers[i].value, "0");
            has_upload_offset = true;
        }
    }
    TEST_CHECK(has_tus_resumable);
    TEST_CHECK(has_content_type);
    TEST_CHECK(has_upload_offset);

    sentry__prepared_http_request_free(req);
    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
    sentry__dsn_decref(dsn);
}

static bool
tus_mock_send(void *client, sentry_prepared_http_request_t *req,
    sentry_http_response_t *resp)
{
    (void)client;
    (void)req;
    resp->status_code = 201;
    resp->location = sentry__string_clone("https://sentry.invalid/upload/abc");
    return true;
}

SENTRY_TEST(tus_file_attachment_preserves_original)
{
#if defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_NX)            \
    || defined(SENTRY_PLATFORM_PS) || defined(SENTRY_PLATFORM_XBOX)
    SKIP_TEST();
#else
    const char *test_file_str
        = SENTRY_TEST_PATH_PREFIX "sentry_test_tus_preserve";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);
    create_large_test_file(test_file_str);

    sentry_transport_t *transport
        = sentry__http_transport_new(NULL, tus_mock_send);
    TEST_CHECK(!!transport);
    sentry__client_report_reset();

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(options, transport);
    sentry_options_set_enable_large_attachments(options, 1);
    sentry_options_add_attachment(options, test_file_str);
    sentry_init(options);

    sentry_capture_event(
        sentry_value_new_message_event(SENTRY_LEVEL_INFO, NULL, "test"));

    sentry_close();

    TEST_CHECK(sentry__path_is_file(test_file_path));
    sentry_client_report_t report = { { 0 } };
    TEST_CHECK(sentry__client_report_save(&report));
    TEST_CHECK_INT_EQUAL(report.counts[SENTRY_DISCARD_REASON_SEND_ERROR]
                                      [SENTRY_DATA_CATEGORY_ATTACHMENT],
        1);
    sentry__client_report_reset();

    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
#endif
}

typedef struct {
    int create_count;
    int envelope_count;
} tus_create_failure_state_t;

static bool
tus_create_failure_send(void *client, sentry_prepared_http_request_t *req,
    sentry_http_response_t *resp)
{
    tus_create_failure_state_t *state = client;

    if (strcmp(req->method, "POST") == 0 && !req->body && !req->body_path) {
        state->create_count++;
        resp->status_code = 404;
        return true;
    }
    if (strcmp(req->method, "POST") == 0 && req->body) {
        state->envelope_count++;
        resp->status_code = 200;
        return true;
    }
    return false;
}

SENTRY_TEST(tus_upload_error)
{
#if defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_NX)            \
    || defined(SENTRY_PLATFORM_PS) || defined(SENTRY_PLATFORM_XBOX)
    SKIP_TEST();
#else
    const char *file1_str = SENTRY_TEST_PATH_PREFIX "sentry_test_tus_error_1";
    const char *file2_str = SENTRY_TEST_PATH_PREFIX "sentry_test_tus_error_2";
    sentry_path_t *file1_path = sentry__path_from_str(file1_str);
    sentry_path_t *file2_path = sentry__path_from_str(file2_str);
    create_large_test_file(file1_str);
    create_large_test_file(file2_str);

    tus_create_failure_state_t state = { 0 };
    sentry_transport_t *transport
        = sentry__http_transport_new(&state, tus_create_failure_send);
    TEST_ASSERT(!!transport);

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(options, transport);
    sentry_options_set_enable_large_attachments(options, 1);
    sentry_options_add_attachment(options, file1_str);
    sentry_options_add_attachment(options, file2_str);
    sentry_init(options);

    sentry_capture_event(
        sentry_value_new_message_event(SENTRY_LEVEL_INFO, NULL, "test"));

    sentry_close();

    TEST_CHECK_INT_EQUAL(state.create_count, 2);
    TEST_CHECK_INT_EQUAL(state.envelope_count, 1);

    sentry__client_report_reset();
    sentry__path_remove(file1_path);
    sentry__path_remove(file2_path);
    sentry__path_free(file1_path);
    sentry__path_free(file2_path);
#endif
}

// Mock that drives a full TUS round-trip: returns a relative `Location` from
// the create POST (per spec), 204 from the upload PATCH, and captures the
// final envelope POST body so the test can inspect the placeholder payload.
typedef struct {
    char *captured_body;
    size_t captured_body_len;
} tus_capture_state_t;

static const char *TUS_RELATIVE_LOCATION
    = "/api/42/upload/019db3e0/?length=104857600&signature=test";

static bool
tus_capture_send(void *client, sentry_prepared_http_request_t *req,
    sentry_http_response_t *resp)
{
    tus_capture_state_t *cap = client;

    // TUS create: POST with no body
    if (strcmp(req->method, "POST") == 0 && !req->body && !req->body_path) {
        resp->status_code = 201;
        resp->location = sentry__string_clone(TUS_RELATIVE_LOCATION);
        return true;
    }
    // TUS upload: PATCH with body_path
    if (strcmp(req->method, "PATCH") == 0 && req->body_path) {
        resp->status_code = 204;
        return true;
    }
    // Envelope POST: capture body
    if (strcmp(req->method, "POST") == 0 && req->body) {
        sentry_free(cap->captured_body);
        cap->captured_body = sentry_malloc(req->body_len);
        memcpy(cap->captured_body, req->body, req->body_len);
        cap->captured_body_len = req->body_len;
        resp->status_code = 200;
        return true;
    }
    return false;
}

SENTRY_TEST(tus_placeholder_uses_raw_location)
{
#if defined(SENTRY_PLATFORM_ANDROID) || defined(SENTRY_PLATFORM_NX)            \
    || defined(SENTRY_PLATFORM_PS) || defined(SENTRY_PLATFORM_XBOX)
    SKIP_TEST();
#else
    // Sparse 100 MiB file forces the attachment through the TUS path.
    const char *test_file_str
        = SENTRY_TEST_PATH_PREFIX "sentry_test_tus_raw_location";
    sentry_path_t *test_file_path = sentry__path_from_str(test_file_str);
    create_large_test_file(test_file_str);

    tus_capture_state_t cap = { 0 };
    sentry_transport_t *transport
        = sentry__http_transport_new(&cap, tus_capture_send);
    TEST_ASSERT(!!transport);

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(options, transport);
    sentry_options_set_enable_large_attachments(options, 1);
    sentry_options_add_attachment(options, test_file_str);
    sentry_init(options);

    sentry_capture_event(
        sentry_value_new_message_event(SENTRY_LEVEL_INFO, NULL, "test"));

    sentry_close();

    TEST_ASSERT(!!cap.captured_body);
    sentry_envelope_t *parsed
        = sentry_envelope_deserialize(cap.captured_body, cap.captured_body_len);
    TEST_ASSERT(!!parsed);

    // Spec requires `event_id` on the envelope header for placeholders.
    sentry_value_t event_id_val
        = sentry_envelope_get_header(parsed, "event_id");
    TEST_CHECK(sentry_value_get_type(event_id_val) == SENTRY_VALUE_TYPE_STRING);

    bool found_ref = false;
    size_t count = sentry__envelope_get_item_count(parsed);
    for (size_t i = 0; i < count; i++) {
        sentry_envelope_item_t *item = sentry__envelope_get_item(parsed, i);
        sentry_attachment_ref_t ref;
        if (!sentry__envelope_item_get_attachment_ref(item, &ref)) {
            continue;
        }
        TEST_CHECK_STRING_EQUAL(ref.location, TUS_RELATIVE_LOCATION);
        sentry__attachment_ref_cleanup(&ref);
        found_ref = true;
    }
    TEST_CHECK(found_ref);

    sentry_envelope_free(parsed);
    sentry_free(cap.captured_body);
    sentry__path_remove(test_file_path);
    sentry__path_free(test_file_path);
#endif
}
