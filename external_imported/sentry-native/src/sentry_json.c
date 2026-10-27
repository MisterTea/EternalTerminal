#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wsign-conversion"
#endif
#include "../vendor/jsmn.h"
#ifdef __clang__
#    pragma clang diagnostic pop
#endif

#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_json.h"
#include "sentry_string.h"
#include "sentry_utils.h"
#include "sentry_value.h"

#define SENTRY_JSON_MAX_DEPTH 64

struct sentry_jsonwriter_s {
    sentry_writer_t *writer;
    uint64_t want_comma;
    uint32_t depth;
    bool last_was_key;
    bool owns_writer;
    bool failed;
};

static sentry_jsonwriter_t *
jsonwriter_new(sentry_writer_t *writer, bool owns_writer)
{
    if (!writer) {
        return NULL;
    }

    sentry_jsonwriter_t *rv = SENTRY_MAKE(sentry_jsonwriter_t);
    if (!rv) {
        if (owns_writer) {
            sentry__writer_free(writer);
        }
        return NULL;
    }

    rv->writer = writer;
    rv->want_comma = 0;
    rv->depth = 0;
    rv->last_was_key = false;
    rv->owns_writer = owns_writer;
    rv->failed = false;
    return rv;
}

sentry_jsonwriter_t *
sentry__jsonwriter_new_writer(sentry_writer_t *writer)
{
    return jsonwriter_new(writer, false);
}

sentry_jsonwriter_t *
sentry__jsonwriter_new_sb(sentry_stringbuilder_t *sb)
{
    return jsonwriter_new(sentry__writer_new_sb(sb), true);
}

sentry_jsonwriter_t *
sentry__jsonwriter_new_fw(sentry_filewriter_t *fw)
{
    return jsonwriter_new(sentry__writer_new_filewriter(fw, false), true);
}

void
sentry__jsonwriter_free(sentry_jsonwriter_t *jw)
{
    if (!jw) {
        return;
    }
    if (jw->owns_writer) {
        sentry__writer_free(jw->writer);
    }
    sentry_free(jw);
}

void
sentry__jsonwriter_reset(sentry_jsonwriter_t *jw)
{
    if (!jw) {
        return;
    }

    jw->want_comma = 0;
    jw->depth = 0;
    jw->last_was_key = false;
}

bool
sentry__jsonwriter_has_failed(const sentry_jsonwriter_t *jw)
{
    return !jw || jw->failed || sentry__writer_has_failed(jw->writer);
}

char *
sentry__jsonwriter_into_string(sentry_jsonwriter_t *jw, size_t *len_out)
{
    if (!jw) {
        if (len_out) {
            *len_out = 0;
        }
        return NULL;
    }

    sentry_writer_t *writer = jw->writer;
    bool owns_writer = jw->owns_writer;
    bool failed = sentry__jsonwriter_has_failed(jw);
    jw->writer = NULL;
    sentry_free(jw);

    if (!owns_writer || failed) {
        if (len_out) {
            *len_out = 0;
        }
        if (owns_writer) {
            sentry__writer_free(writer);
        }
        return NULL;
    }
    return sentry__writer_into_string(writer, len_out);
}

static bool
at_max_depth(const sentry_jsonwriter_t *jw)
{
    return jw->depth >= SENTRY_JSON_MAX_DEPTH;
}

static void
set_comma(sentry_jsonwriter_t *jw, bool val)
{
    if (at_max_depth(jw)) {
        return;
    }
    if (val) {
        jw->want_comma |= 1ULL << jw->depth;
    } else {
        jw->want_comma &= ~(1ULL << jw->depth);
    }
}

static void
write_char(sentry_jsonwriter_t *jw, char c)
{
    if (!sentry__writer_write_char(jw->writer, c)) {
        jw->failed = true;
    }
}

static void
write_str(sentry_jsonwriter_t *jw, const char *str)
{
    if (!sentry__writer_write(jw->writer, str, strlen(str))) {
        jw->failed = true;
    }
}

static void
write_buf(sentry_jsonwriter_t *jw, const char *buf, size_t len)
{
    if (!sentry__writer_write(jw->writer, buf, len)) {
        jw->failed = true;
    }
}

// The Lookup table and algorithm below are adapted from:
// https://github.com/serde-rs/json/blob/977975ee650829a1f3c232cd5f641a7011bdce1d/src/ser.rs#L2079-L2145

// Lookup table of escape sequences. `0` means no need to escape, and `1` means
// that escaping is needed.
static unsigned char needs_escaping[256] = {
    // 1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 0
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 1
    0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 2
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 3
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 4
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, // 5
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 6
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 7
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 8
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 9
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // A
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // B
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // C
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // D
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // E
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // F
};

static void
write_json_str(sentry_jsonwriter_t *jw, const char *str)
{
    // using unsigned here because utf-8 is > 127 :-)
    const unsigned char *ptr = (const unsigned char *)str;
    const unsigned char *start = ptr;
    for (; *ptr && !sentry__jsonwriter_has_failed(jw); ptr++) {
        if (!needs_escaping[*ptr]) {
            continue;
        }

        size_t len = (size_t)(ptr - start);
        if (len) {
            write_buf(jw, (const char *)start, len);
        }

        if (sentry__jsonwriter_has_failed(jw)) {
            return;
        }

        switch (*ptr) {
        case '\\':
            write_str(jw, "\\\\");
            break;
        case '"':
            write_str(jw, "\\\"");
            break;
        case '\b':
            write_str(jw, "\\b");
            break;
        case '\f':
            write_str(jw, "\\f");
            break;
        case '\n':
            write_str(jw, "\\n");
            break;
        case '\r':
            write_str(jw, "\\r");
            break;
        case '\t':
            write_str(jw, "\\t");
            break;
        default:
            // See https://tools.ietf.org/html/rfc8259#section-7
            // We only need to escape the control characters, otherwise we
            // assume that `str` is valid utf-8
            if (*ptr < 32) {
                char buf[10];
                snprintf(buf, sizeof(buf), "\\u%04x", *ptr);
                write_str(jw, buf);
            } else {
                write_char(jw, (char)*ptr);
            }
        }

        if (sentry__jsonwriter_has_failed(jw)) {
            return;
        }

        start = ptr + 1;
    }

    size_t len = (size_t)(ptr - start);
    if (len) {
        write_buf(jw, (const char *)start, len);
    }
}

static bool
can_write_item(sentry_jsonwriter_t *jw)
{
    if (at_max_depth(jw) || sentry__jsonwriter_has_failed(jw)) {
        return false;
    }
    if (jw->last_was_key) {
        jw->last_was_key = false;
        return true;
    }
    if ((jw->want_comma >> jw->depth) & 1) {
        write_char(jw, ',');
    } else {
        set_comma(jw, true);
    }
    return !sentry__jsonwriter_has_failed(jw);
}

void
sentry__jsonwriter_write_null(sentry_jsonwriter_t *jw)
{
    if (can_write_item(jw)) {
        write_str(jw, "null");
    }
}

void
sentry__jsonwriter_write_bool(sentry_jsonwriter_t *jw, bool val)
{
    if (can_write_item(jw)) {
        write_str(jw, val ? "true" : "false");
    }
}

void
sentry__jsonwriter_write_int32(sentry_jsonwriter_t *jw, int32_t val)
{
    if (can_write_item(jw)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRId32, val);
        write_str(jw, buf);
    }
}

void
sentry__jsonwriter_write_int64(sentry_jsonwriter_t *jw, int64_t val)
{
    if (can_write_item(jw)) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%" PRId64, val);
        write_str(jw, buf);
    }
}

void
sentry__jsonwriter_write_uint64(sentry_jsonwriter_t *jw, uint64_t val)
{
    if (can_write_item(jw)) {
        char buf[26];
        snprintf(buf, sizeof(buf), "%" PRIu64, val);
        write_str(jw, buf);
    }
}

void
sentry__jsonwriter_write_double(sentry_jsonwriter_t *jw, double val)
{
    if (can_write_item(jw)) {
        char buf[24];
        // The MAX_SAFE_INTEGER is 9007199254740991, which has 16 digits
        int written = sentry__snprintf_c(buf, sizeof(buf), "%.16g", val);
        // print `null` if we have printf issues or a non-finite double, which
        // can't be represented in JSON.
        if (written < 0 || written >= (int)sizeof(buf) || !isfinite(val)) {
            write_str(jw, "null");
        } else {
            buf[written] = '\0';
            write_str(jw, buf);
        }
    }
}

void
sentry__jsonwriter_write_str(sentry_jsonwriter_t *jw, const char *val)
{
    if (!val) {
        sentry__jsonwriter_write_null(jw);
        return;
    }
    if (can_write_item(jw)) {
        write_char(jw, '"');
        write_json_str(jw, val);
        write_char(jw, '"');
    }
}

void
sentry__jsonwriter_write_uuid(
    sentry_jsonwriter_t *jw, const sentry_uuid_t *uuid)
{
    if (!uuid) {
        sentry__jsonwriter_write_null(jw);
        return;
    }
    char buf[37];
    sentry_uuid_as_string(uuid, buf);
    sentry__jsonwriter_write_str(jw, buf);
}

void
sentry__jsonwriter_write_usec_timestamp(sentry_jsonwriter_t *jw, uint64_t time)
{
    char *formatted = sentry__usec_time_to_iso8601(time);
    sentry__jsonwriter_write_str(jw, formatted);
    sentry_free(formatted);
}

void
sentry__jsonwriter_write_key(sentry_jsonwriter_t *jw, const char *val)
{
    if (can_write_item(jw)) {
        write_char(jw, '"');
        write_json_str(jw, val);
        write_char(jw, '"');
        write_char(jw, ':');
        jw->last_was_key = true;
    }
}

void
sentry__jsonwriter_write_list_start(sentry_jsonwriter_t *jw)
{
    if (can_write_item(jw)) {
        write_char(jw, '[');
    }
    jw->depth += 1;
    set_comma(jw, false);
}

void
sentry__jsonwriter_write_list_end(sentry_jsonwriter_t *jw)
{
    jw->depth -= 1;
    if (!at_max_depth(jw)) {
        write_char(jw, ']');
    }
}

void
sentry__jsonwriter_write_object_start(sentry_jsonwriter_t *jw)
{
    if (can_write_item(jw)) {
        write_char(jw, '{');
    }
    jw->depth += 1;
    set_comma(jw, false);
}

void
sentry__jsonwriter_write_object_end(sentry_jsonwriter_t *jw)
{
    jw->depth -= 1;
    if (!at_max_depth(jw)) {
        write_char(jw, '}');
    }
}

static int32_t
read_escaped_unicode_char(const char *buf)
{
    size_t i;
    int32_t uchar = 0;

    for (i = 0; i < 4; i++) {
        char c = *buf++;
        uchar <<= 4;
        if (c >= '0' && c <= '9') {
            uchar |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            uchar |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            uchar |= c - 'A' + 10;
        } else {
            return (int32_t)-1;
        }
    }

    return uchar;
}

static bool
decode_string_inplace(char *buf)
{
    const char *input = buf;
    char *output = buf;

#define SIMPLE_ESCAPE(Char, Rep)                                               \
    case Char:                                                                 \
        *output++ = Rep;                                                       \
        break

    while (*input) {
        char c = *input++;
        if (c != '\\') {
            *output++ = c;
            continue;
        }
        switch (*input++) {
            SIMPLE_ESCAPE('"', '"');
            SIMPLE_ESCAPE('\\', '\\');
            SIMPLE_ESCAPE('/', '/');
            SIMPLE_ESCAPE('b', '\b');
            SIMPLE_ESCAPE('f', '\f');
            SIMPLE_ESCAPE('n', '\n');
            SIMPLE_ESCAPE('r', '\r');
            SIMPLE_ESCAPE('t', '\t');
        case 'u': {
            int32_t uchar = read_escaped_unicode_char(input);
            if (uchar == (int32_t)-1) {
                return false;
            }
            input += 4;

            if (sentry__is_lead_surrogate(uchar)) {
                uint16_t lead = (uint16_t)uchar;
                if (input[0] != '\\' || input[1] != 'u') {
                    return false;
                }
                input += 2;
                int32_t trail = read_escaped_unicode_char(input);
                if (trail == (int32_t)-1
                    || !sentry__is_trail_surrogate(trail)) {
                    return false;
                }
                input += 4;
                uchar = sentry__surrogate_value(lead, trail);
            } else if (sentry__is_trail_surrogate(uchar)) {
                return false;
            }

            if (uchar) {
                output += sentry__unichar_to_utf8((uint32_t)uchar, output);
            }
            break;
        }
        default:
            return false;
        }
    }

#undef SIMPLE_ESCAPE

    *output = 0;
    return true;
}

static size_t
tokens_to_value(jsmntok_t *tokens, size_t token_count, const char *buf,
    size_t depth, sentry_value_t *value_out)
{
    size_t offset = 0;

#define POP() (offset < token_count ? &tokens[offset++] : NULL)
#define NESTED_PARSE(Target)                                                   \
    do {                                                                       \
        size_t child_consumed = tokens_to_value(                               \
            tokens + offset, token_count - offset, buf, depth + 1, Target);    \
        if (child_consumed == (size_t)-1) {                                    \
            goto error;                                                        \
        }                                                                      \
        offset += child_consumed;                                              \
    } while (0)

    jsmntok_t *root = POP();
    sentry_value_t rv = sentry_value_new_null();

    if (!root) {
        goto error;
    }

    switch (root->type) {
    case JSMN_PRIMITIVE: {
        switch (buf[root->start]) {
        case 't':
            rv = sentry_value_new_bool(true);
            break;
        case 'f':
            rv = sentry_value_new_bool(false);
            break;
        case 'n':
            rv = sentry_value_new_null();
            break;
        default: {
            bool success = false;
            char *endptr;
            errno = 0;

            if (buf[root->start] == '-') {
                const long long ll_val
                    = strtoll(buf + root->start, &endptr, 10);
                if (endptr == buf + root->end && errno == 0) {
                    if (ll_val >= INT32_MIN && ll_val <= INT32_MAX) {
                        rv = sentry_value_new_int32((int32_t)ll_val);
                    } else {
                        rv = sentry_value_new_int64((int64_t)ll_val);
                    }
                    success = true;
                }
            } else {
                const unsigned long long ull_val
                    = strtoull(buf + root->start, &endptr, 10);
                if (endptr == buf + root->end && errno == 0) {
                    if (ull_val <= INT32_MAX) {
                        rv = sentry_value_new_int32((int32_t)ull_val);
                    } else if (ull_val <= INT64_MAX) {
                        rv = sentry_value_new_int64((int64_t)ull_val);
                    } else {
                        rv = sentry_value_new_uint64((uint64_t)ull_val);
                    }
                    success = true;
                }
            }

            // Both failed, fallback to double
            if (!success) {
                double val = sentry__strtod_c(buf + root->start, NULL);
                rv = sentry_value_new_double(val);
            }
            break;
        }
        }
        break;
    }
    case JSMN_STRING: {
        char *string = sentry__string_clone_n_unchecked(
            buf + root->start, (size_t)(root->end - root->start));
        if (decode_string_inplace(string)) {
            rv = sentry__value_new_string_owned(string);
        } else {
            sentry_free(string);
            rv = sentry_value_new_null();
        }
        break;
    }
    case JSMN_OBJECT: {
        if (depth >= SENTRY_JSON_MAX_DEPTH) {
            goto error;
        }
        rv = sentry_value_new_object();
        for (int i = 0; i < root->size; i++) {
            jsmntok_t *token = POP();
            if (!token || token->type != JSMN_STRING) {
                goto error;
            }

            sentry_value_t child;
            NESTED_PARSE(&child);

            char *key = sentry__string_clone_n_unchecked(
                buf + token->start, (size_t)(token->end - token->start));
            if (decode_string_inplace(key)) {
                sentry_value_set_by_key(rv, key, child);
            } else {
                sentry_value_decref(child);
            }
            sentry_free(key);
        }
        break;
    }
    case JSMN_ARRAY: {
        if (depth >= SENTRY_JSON_MAX_DEPTH) {
            goto error;
        }
        rv = sentry_value_new_list();
        for (int i = 0; i < root->size; i++) {
            sentry_value_t child;
            NESTED_PARSE(&child);
            sentry_value_append(rv, child);
        }
        break;
    }
    case JSMN_UNDEFINED:
        break;
    }

#undef POP
#undef NESTED_PARSE

    *value_out = rv;
    return offset;

error:
    sentry_value_decref(rv);
    return (size_t)-1;
}

sentry_value_t
sentry__value_from_json(const char *buf, size_t buflen)
{
    int token_count;
    jsmn_parser jsmn_p;

    jsmn_init(&jsmn_p);
    token_count = jsmn_parse(&jsmn_p, buf, buflen, NULL, 0);
    if (token_count <= 0
        || (size_t)token_count > SIZE_MAX / sizeof(jsmntok_t)) {
        return sentry_value_new_null();
    }

    jsmntok_t *tokens
        = sentry_malloc(sizeof(jsmntok_t) * (size_t)(token_count));
    if (!tokens) {
        return sentry_value_new_null();
    }
    jsmn_init(&jsmn_p);
    token_count
        = jsmn_parse(&jsmn_p, buf, buflen, tokens, (unsigned int)(token_count));
    if (token_count <= 0) {
        sentry_free(tokens);
        return sentry_value_new_null();
    }

    sentry_value_t value_out;
    size_t tokens_consumed
        = tokens_to_value(tokens, (size_t)token_count, buf, 0, &value_out);
    sentry_free(tokens);

    if (tokens_consumed == (size_t)token_count) {
        return value_out;
    } else {
        return sentry_value_new_null();
    }
}
