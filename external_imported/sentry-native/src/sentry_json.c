#include <assert.h>
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

typedef struct {
    void (*free)(sentry_jsonwriter_t *writer);
    void (*write_str)(sentry_jsonwriter_t *writer, const char *str);
    void (*write_buf)(sentry_jsonwriter_t *writer, const char *buf, size_t len);
    void (*write_char)(sentry_jsonwriter_t *writer, char c);
    char *(*into_string)(sentry_jsonwriter_t *jw, size_t *len_out);
} sentry_jsonwriter_ops_t;

struct sentry_jsonwriter_s {
    union {
        sentry_stringbuilder_t *sb;
        sentry_filewriter_t *fw;
    } output;
    uint64_t want_comma;
    uint32_t depth;
    bool last_was_key;
    bool owns_sb;
    sentry_jsonwriter_ops_t *ops;
};

static void
jsonwriter_free_sb(sentry_jsonwriter_t *jw)
{
    if (!jw) {
        return;
    }
    if (jw->owns_sb) {
        sentry__stringbuilder_cleanup(jw->output.sb);
        sentry_free(jw->output.sb);
    }
    sentry_free(jw);
}

static void
jsonwriter_free_file(sentry_jsonwriter_t *jw)
{
    if (!jw) {
        return;
    }
    sentry_free(jw);
}

static void
write_char_sb(sentry_jsonwriter_t *jw, char c)
{
    sentry__stringbuilder_append_char(jw->output.sb, c);
}

static void
write_str_sb(sentry_jsonwriter_t *jw, const char *str)
{
    sentry__stringbuilder_append(jw->output.sb, str);
}

static void
write_buf_sb(sentry_jsonwriter_t *jw, const char *buf, size_t len)
{
    sentry__stringbuilder_append_buf(jw->output.sb, buf, len);
}

static void
write_char_file(sentry_jsonwriter_t *jw, char c)
{
    sentry__filewriter_write(jw->output.fw, &c, sizeof(char));
}

static void
write_str_file(sentry_jsonwriter_t *jw, const char *str)
{
    sentry__filewriter_write(jw->output.fw, str, sizeof(char) * strlen(str));
}

static void
write_buf_file(sentry_jsonwriter_t *jw, const char *buf, size_t len)
{
    sentry__filewriter_write(jw->output.fw, buf, len);
}

static char *
into_string_sb(sentry_jsonwriter_t *jw, size_t *len_out)
{
    char *rv = NULL;
    sentry_stringbuilder_t *sb = jw->output.sb;
    if (len_out) {
        *len_out = sb->len;
    }
    rv = sentry__stringbuilder_into_string(sb);
    sentry__jsonwriter_free(jw);
    return rv;
}

static char *
into_string_file(sentry_jsonwriter_t *UNUSED(jw), size_t *len_out)
{
    UNREACHABLE("A file-based jsonwriter can't convert into string");

    *len_out = 0;
    return NULL;
}

static sentry_jsonwriter_ops_t sb_ops = {
    .write_char = write_char_sb,
    .write_str = write_str_sb,
    .write_buf = write_buf_sb,
    .free = jsonwriter_free_sb,
    .into_string = into_string_sb,
};

sentry_jsonwriter_t *
sentry__jsonwriter_new_sb(sentry_stringbuilder_t *sb)
{
    bool owns_sb = false;
    if (!sb) {
        sb = SENTRY_MAKE(sentry_stringbuilder_t);
        if (!sb) {
            return NULL;
        }
        owns_sb = true;
        sentry__stringbuilder_init(sb);
    }
    sentry_jsonwriter_t *rv = SENTRY_MAKE(sentry_jsonwriter_t);
    if (!rv) {
        if (owns_sb) {
            sentry_free(sb);
        }
        return NULL;
    }

    rv->output.sb = sb;
    rv->want_comma = 0;
    rv->depth = 0;
    rv->last_was_key = 0;
    rv->owns_sb = owns_sb;
    rv->ops = &sb_ops;
    return rv;
}

static sentry_jsonwriter_ops_t file_ops = {
    .free = jsonwriter_free_file,
    .write_char = write_char_file,
    .write_str = write_str_file,
    .write_buf = write_buf_file,
    .into_string = into_string_file,
};

sentry_jsonwriter_t *
sentry__jsonwriter_new_fw(sentry_filewriter_t *fw)
{
    bool owns_sb = false;
    sentry_jsonwriter_t *rv = SENTRY_MAKE(sentry_jsonwriter_t);
    if (!rv) {
        return NULL;
    }

    rv->output.fw = fw;
    rv->want_comma = 0;
    rv->depth = 0;
    rv->last_was_key = 0;
    rv->owns_sb = owns_sb;
    rv->ops = &file_ops;
    return rv;
}

void
sentry__jsonwriter_free(sentry_jsonwriter_t *jw)
{
    jw->ops->free(jw);
}

void
sentry__jsonwriter_reset(sentry_jsonwriter_t *jw)
{
    jw->want_comma = 0;
    jw->depth = 0;
    jw->last_was_key = 0;
}

char *
sentry__jsonwriter_into_string(sentry_jsonwriter_t *jw, size_t *len_out)
{
    return jw->ops->into_string(jw, len_out);
}

static bool
at_max_depth(const sentry_jsonwriter_t *jw)
{
    return jw->depth >= 64;
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
    jw->ops->write_char(jw, c);
}

static void
write_str(sentry_jsonwriter_t *jw, const char *str)
{
    jw->ops->write_str(jw, str);
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
    write_char(jw, '"');

    const unsigned char *start = ptr;
    for (; *ptr; ptr++) {
        if (!needs_escaping[*ptr]) {
            continue;
        }

        size_t len = (size_t)(ptr - start);
        if (len) {
            jw->ops->write_buf(jw, (const char *)start, len);
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

        start = ptr + 1;
    }

    size_t len = (size_t)(ptr - start);
    if (len) {
        jw->ops->write_buf(jw, (const char *)start, len);
    }

    write_char(jw, '"');
}

static bool
can_write_item(sentry_jsonwriter_t *jw)
{
    if (at_max_depth(jw)) {
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
    return true;
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
        write_json_str(jw, val);
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
        write_json_str(jw, val);
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
    sentry_value_t *value_out)
{
    size_t offset = 0;

#define POP() (offset < token_count ? &tokens[offset++] : NULL)
#define NESTED_PARSE(Target)                                                   \
    do {                                                                       \
        size_t child_consumed = tokens_to_value(                               \
            tokens + offset, token_count - offset, buf, Target);               \
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
            double val = sentry__strtod_c(buf + root->start, NULL);
#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wfloat-equal"
#endif
            if (val == (double)(int32_t)val) {
#ifdef __clang__
#    pragma clang diagnostic pop
#endif
                rv = sentry_value_new_int32((int32_t)val);
            } else {
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
    if (token_count <= 0) {
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
        = tokens_to_value(tokens, (size_t)token_count, buf, &value_out);
    sentry_free(tokens);

    if (tokens_consumed == (size_t)token_count) {
        return value_out;
    } else {
        return sentry_value_new_null();
    }
}
