#include "sentry_attachment.h"
#include "sentry_envelope.h"
#include "sentry_path.h"
#include "sentry_scope.h"
#include "sentry_string.h"
#include "sentry_testsupport.h"

typedef struct {
    uint64_t called;
    sentry_stringbuilder_t serialized_envelope;
} sentry_attachments_testdata_t;

static void
send_envelope_test_attachments(const sentry_envelope_t *envelope, void *_data)
{
    sentry_attachments_testdata_t *data = _data;
    data->called += 1;
    sentry__envelope_serialize_into_stringbuilder(
        envelope, &data->serialized_envelope);
}

SENTRY_TEST(lazy_attachments)
{
    sentry_attachments_testdata_t testdata;
    testdata.called = 0;
    sentry__stringbuilder_init(&testdata.serialized_envelope);

    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_set_auto_session_tracking(options, false);
    sentry_options_set_dsn(options, "https://foo@sentry.invalid/42");
    sentry_options_set_transport(options,
        sentry_new_function_transport(
            send_envelope_test_attachments, &testdata));
    char rel[] = { 't', 'e', 's', 't' };
    sentry_options_set_release_n(options, rel, sizeof(rel));

    sentry_options_add_attachment(
        options, SENTRY_TEST_PATH_PREFIX ".existing-file-attachment");
    sentry_options_add_attachment(
        options, SENTRY_TEST_PATH_PREFIX ".non-existing-file-attachment");
    sentry_path_t *existing = sentry__path_from_str(
        SENTRY_TEST_PATH_PREFIX ".existing-file-attachment");
    sentry_path_t *non_existing = sentry__path_from_str(
        SENTRY_TEST_PATH_PREFIX ".non-existing-file-attachment");

    sentry_init(options);

    sentry__path_write_buffer(existing, "foo", 3);
    sentry_capture_event(sentry_value_new_message_event(
        SENTRY_LEVEL_INFO, "root", "Hello World!"));

    char *serialized
        = sentry_stringbuilder_take_string(&testdata.serialized_envelope);
    TEST_CHECK(strstr(serialized, "\"release\":\"test\"") != NULL);
    TEST_CHECK(strstr(serialized,
                   "{\"type\":\"attachment\",\"length\":3,"
                   "\"filename\":\".existing-file-attachment\"}\n"
                   "foo")
        != NULL);
    TEST_CHECK(
        strstr(serialized, "\"filename\":\".non-existing-file-attachment\"")
        == NULL);
    sentry_free(serialized);

    sentry__path_write_buffer(existing, "foobar", 6);
    sentry__path_write_buffer(non_existing, "it exists", 9);
    sentry_capture_event(sentry_value_new_message_event(
        SENTRY_LEVEL_INFO, "root", "Hello World!"));

    serialized
        = sentry_stringbuilder_take_string(&testdata.serialized_envelope);
    TEST_CHECK(strstr(serialized,
                   "{\"type\":\"attachment\",\"length\":6,"
                   "\"filename\":\".existing-file-attachment\"}\n"
                   "foobar")
        != NULL);
    TEST_CHECK(strstr(serialized,
                   "{\"type\":\"attachment\",\"length\":9,"
                   "\"filename\":\".non-existing-file-attachment\"}\n"
                   "it exists")
        != NULL);
    sentry_free(serialized);

    sentry_close();

    sentry__path_remove(existing);
    sentry__path_remove(non_existing);
    sentry__path_free(existing);
    sentry__path_free(non_existing);

    TEST_CHECK_INT_EQUAL(testdata.called, 2);
}

SENTRY_TEST(attachments_add_dedupe)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_add_attachment(options, SENTRY_TEST_PATH_PREFIX ".a.txt");
    sentry_options_add_attachment(options, SENTRY_TEST_PATH_PREFIX ".b.txt");

    sentry_init(options);

    sentry_attach_file(SENTRY_TEST_PATH_PREFIX ".a.txt");
    sentry_attach_file(SENTRY_TEST_PATH_PREFIX ".b.txt");
    sentry_attach_file(SENTRY_TEST_PATH_PREFIX ".c.txt");
#ifdef SENTRY_PLATFORM_WINDOWS
    sentry_attach_filew(L".a.txt");
    sentry_attach_filew(L".b.txt");
    sentry_attach_filew(L".c.txt");
#endif

    sentry_path_t *path_a
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".a.txt");
    sentry_path_t *path_b
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".b.txt");
    sentry_path_t *path_c
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".c.txt");

    sentry__path_write_buffer(path_a, "aaa", 3);
    sentry__path_write_buffer(path_b, "bbb", 3);
    sentry__path_write_buffer(path_c, "ccc", 3);

    sentry_envelope_t *envelope = sentry__envelope_new();
    SENTRY_WITH_SCOPE (scope) {
        sentry__envelope_add_attachments(envelope, scope->attachments);
    }
    char *serialized = sentry_envelope_serialize(envelope, NULL);
    sentry_envelope_free(envelope);

    TEST_CHECK_STRING_EQUAL(serialized,
        "{}\n"
        "{\"type\":\"attachment\",\"length\":3,\"filename\":\".a.txt\"}\naaa\n"
        "{\"type\":\"attachment\",\"length\":3,\"filename\":\".b.txt\"}\nbbb\n"
        "{\"type\":\"attachment\",\"length\":3,\"filename\":\".c.txt\"}"
        "\nccc");

    sentry_free(serialized);

    sentry_close();

    sentry__path_remove(path_a);
    sentry__path_remove(path_b);
    sentry__path_remove(path_c);

    sentry__path_free(path_a);
    sentry__path_free(path_b);
    sentry__path_free(path_c);
}

SENTRY_TEST(attachments_add_remove)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_options_add_attachment(options, SENTRY_TEST_PATH_PREFIX ".a.txt");
    sentry_options_add_attachment(options, SENTRY_TEST_PATH_PREFIX ".c.txt");
    sentry_options_add_attachment(options, SENTRY_TEST_PATH_PREFIX ".b.txt");

    sentry_init(options);

    sentry_attachment_t *attachment_c
        = sentry_attach_file(SENTRY_TEST_PATH_PREFIX ".c.txt");
    sentry_attachment_t *attachment_d
        = sentry_attach_file(SENTRY_TEST_PATH_PREFIX ".d.txt");
#ifdef SENTRY_PLATFORM_WINDOWS
    sentry_attachment_t *attachment_ew = sentry_attach_filew(L".e.txt");
#endif

    sentry_remove_attachment(attachment_c);
    sentry_remove_attachment(attachment_d);
#ifdef SENTRY_PLATFORM_WINDOWS
    sentry_remove_attachment(attachment_ew);
#endif

    sentry_path_t *path_a
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".a.txt");
    sentry_path_t *path_b
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".b.txt");
    sentry_path_t *path_c
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".c.txt");

    sentry__path_write_buffer(path_a, "aaa", 3);
    sentry__path_write_buffer(path_b, "bbb", 3);
    sentry__path_write_buffer(path_c, "ccc", 3);

    sentry_envelope_t *envelope;
    char *serialized;

    envelope = sentry__envelope_new();
    SENTRY_WITH_SCOPE (scope) {
        sentry__envelope_add_attachments(envelope, scope->attachments);
    }
    serialized = sentry_envelope_serialize(envelope, NULL);
    sentry_envelope_free(envelope);

    TEST_CHECK_STRING_EQUAL(serialized,
        "{}\n"
        "{\"type\":\"attachment\",\"length\":3,\"filename\":\".a.txt\"}\naaa\n"
        "{\"type\":\"attachment\",\"length\":3,\"filename\":\".b.txt\"}"
        "\nbbb");

    sentry_free(serialized);

    sentry_close();

    sentry__path_remove(path_a);
    sentry__path_remove(path_b);
    sentry__path_remove(path_c);

    sentry__path_free(path_a);
    sentry__path_free(path_b);
    sentry__path_free(path_c);
}

SENTRY_TEST(attachments_extend)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    sentry_path_t *path_a
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".a.txt");
    sentry_path_t *path_b
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".b.txt");
    sentry_path_t *path_c
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".c.txt");
    sentry_path_t *path_d
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".d.txt");

    sentry__path_write_buffer(path_a, "aaa", 3);
    sentry__path_write_buffer(path_b, "bbb", 3);
    sentry__path_write_buffer(path_c, "ccc", 3);
    sentry__path_write_buffer(path_d, "ddd", 3);

    sentry_attachment_t *attachments_abc = NULL;
    sentry__attachments_add_path(
        &attachments_abc, sentry__path_clone(path_a), ATTACHMENT, NULL);
    sentry__attachments_add_path(
        &attachments_abc, sentry__path_clone(path_b), ATTACHMENT, NULL);
    sentry__attachments_add_path(
        &attachments_abc, sentry__path_clone(path_c), ATTACHMENT, NULL);

    sentry_attachment_t *attachments_bcd = NULL;
    sentry__attachments_add_path(
        &attachments_bcd, sentry__path_clone(path_b), ATTACHMENT, NULL);
    sentry__attachments_add_path(
        &attachments_bcd, sentry__path_clone(path_c), ATTACHMENT, NULL);
    sentry__attachments_add_path(
        &attachments_bcd, sentry__path_clone(path_d), ATTACHMENT, NULL);

    sentry_attachment_t *all_attachments = NULL;
    sentry__attachments_extend(&all_attachments, attachments_abc);
    TEST_CHECK(all_attachments != NULL);

    SENTRY_WITH_SCOPE (scope) {
        sentry_envelope_t *envelope = sentry__envelope_new();
        sentry__envelope_add_attachments(envelope, all_attachments);

        char *serialized = sentry_envelope_serialize(envelope, NULL);

        TEST_CHECK_STRING_EQUAL(serialized,
            "{}\n"
            "{\"type\":\"attachment\",\"length\":3,\"filename\":\".a.txt\"}"
            "\naaa\n"
            "{\"type\":\"attachment\",\"length\":3,\"filename\":\".b.txt\"}"
            "\nbbb\n"
            "{\"type\":\"attachment\",\"length\":3,\"filename\":\".c.txt\"}"
            "\nccc");

        sentry_free(serialized);
        sentry_envelope_free(envelope);
    }

    sentry__attachments_extend(&all_attachments, attachments_bcd);
    TEST_CHECK(all_attachments != NULL);

    SENTRY_WITH_SCOPE (scope) {
        sentry_envelope_t *envelope = sentry__envelope_new();
        sentry__envelope_add_attachments(envelope, all_attachments);

        char *serialized = sentry_envelope_serialize(envelope, NULL);

        TEST_CHECK_STRING_EQUAL(serialized,
            "{}\n"
            "{\"type\":\"attachment\",\"length\":3,\"filename\":\".a.txt\"}"
            "\naaa\n"
            "{\"type\":\"attachment\",\"length\":3,\"filename\":\".b.txt\"}"
            "\nbbb\n"
            "{\"type\":\"attachment\",\"length\":3,\"filename\":\".c.txt\"}"
            "\nccc\n"
            "{\"type\":\"attachment\",\"length\":3,\"filename\":\".d.txt\"}"
            "\nddd");

        sentry_free(serialized);
        sentry_envelope_free(envelope);
    }

    sentry_close();

    sentry__attachments_free(attachments_abc);
    sentry__attachments_free(attachments_bcd);
    sentry__attachments_free(all_attachments);

    sentry__path_remove(path_a);
    sentry__path_remove(path_b);
    sentry__path_remove(path_c);
    sentry__path_remove(path_d);

    sentry__path_free(path_a);
    sentry__path_free(path_b);
    sentry__path_free(path_c);
    sentry__path_free(path_d);
}

SENTRY_TEST(attachment_properties)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    sentry_path_t *path_txt
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".a.txt");
    sentry_path_t *path_html
        = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".b.html");
    sentry_path_t *path_c = sentry__path_from_str(SENTRY_TEST_PATH_PREFIX ".c");

    sentry__path_write_buffer(path_txt, "plain", 5);
    sentry__path_write_buffer(path_html, "<html/>", 7);
    sentry__path_write_buffer(path_c, "int main() {}", 13);

    sentry_attachment_t *attachment_txt
        = sentry_attach_file(SENTRY_TEST_PATH_PREFIX ".a.txt");
    sentry_attachment_set_content_type(attachment_txt, "text/plain");
    sentry_attachment_set_filename(attachment_txt, "A.TXT");

    sentry_attachment_t *attachment_html
        = sentry_attach_file(SENTRY_TEST_PATH_PREFIX ".b.html");
    sentry_attachment_set_content_type(attachment_html, "text/html");
    sentry_attachment_set_filename(attachment_html, "B.HTML");

    sentry_attachment_t *attachment_c
        = sentry_attach_file(SENTRY_TEST_PATH_PREFIX ".c");
    sentry_attachment_set_content_type(attachment_c, NULL);

    SENTRY_WITH_SCOPE (scope) {
        sentry_envelope_t *envelope = sentry__envelope_new();
        sentry__envelope_add_attachments(envelope, scope->attachments);

        char *serialized = sentry_envelope_serialize(envelope, NULL);
        TEST_CHECK_STRING_EQUAL(serialized,
            "{}\n"
            "{\"type\":\"attachment\",\"length\":5,\"content_type\":\"text/"
            "plain\","
            "\"filename\":\"A.TXT\"}\nplain\n"
            "{\"type\":\"attachment\",\"length\":7,\"content_type\":\"text/"
            "html\","
            "\"filename\":\"B.HTML\"}\n<html/>"
            "\n{\"type\":\"attachment\",\"length\":13,\"filename\":\".c\"}\n"
            "int main() {}");
        sentry_free(serialized);
        sentry_envelope_free(envelope);
    }

    sentry_close();

    sentry__path_remove(path_txt);
    sentry__path_remove(path_html);
    sentry__path_remove(path_c);

    sentry__path_free(path_txt);
    sentry__path_free(path_html);
    sentry__path_free(path_c);
}

SENTRY_TEST(attachments_bytes)
{
    SENTRY_TEST_OPTIONS_NEW(options);
    sentry_init(options);

    sentry_attachment_t *attachment_a = sentry_attach_bytes("a", 1, ".a.txt");
    sentry_attachment_t *attachment_b
        = sentry_attach_bytes("b\0b", 3, ".b.txt");
    sentry_attachment_t *attachment_c
        = sentry_attach_bytes("c\0c\0c", 5, ".c.txt");
    sentry_attachment_t *attachment_dupe
        = sentry_attach_bytes("dupe", 4, ".c.txt");

    SENTRY_WITH_SCOPE (scope) {
        sentry_envelope_t *envelope = sentry__envelope_new();
        sentry__envelope_add_attachments(envelope, scope->attachments);

        char *serialized = sentry_envelope_serialize(envelope, NULL);
        TEST_CHECK_STRING_EQUAL(serialized,
            "{}\n"
            "{\"type\":\"attachment\",\"length\":1,\"filename\":\".a.txt\"}"
            "\na\n"
            "{\"type\":\"attachment\",\"length\":3,\"filename\":\".b.txt\"}"
            "\nb\0b\n"
            "{\"type\":\"attachment\",\"length\":5,\"filename\":\".c.txt\"}"
            "\nc\0c\0c"
            "\n{\"type\":\"attachment\",\"length\":5,\"filename\":\".c.txt\"}"
            "\ndupe");
        sentry_free(serialized);

        sentry_envelope_free(envelope);
    }

    sentry_remove_attachment(attachment_b);
    sentry_remove_attachment(attachment_dupe);

    SENTRY_WITH_SCOPE (scope) {
        sentry_envelope_t *envelope = sentry__envelope_new();
        sentry__envelope_add_attachments(envelope, scope->attachments);

        char *serialized = sentry_envelope_serialize(envelope, NULL);
        TEST_CHECK_STRING_EQUAL(serialized,
            "{}\n"
            "{\"type\":\"attachment\",\"length\":1,\"filename\":\".a.txt\"}"
            "\na\n"
            "{\"type\":\"attachment\",\"length\":5,\"filename\":\".c.txt\"}"
            "\nc\0c\0c");
        sentry_free(serialized);

        sentry_envelope_free(envelope);
    }

    sentry_remove_attachment(attachment_a);
    sentry_remove_attachment(attachment_c);

    SENTRY_WITH_SCOPE (scope) {
        sentry_envelope_t *envelope = sentry__envelope_new();
        sentry__envelope_add_attachments(envelope, scope->attachments);

        char *serialized = sentry_envelope_serialize(envelope, NULL);
        TEST_CHECK_STRING_EQUAL(serialized, "{}");
        sentry_free(serialized);

        sentry_envelope_free(envelope);
    }

    sentry_close();
}
