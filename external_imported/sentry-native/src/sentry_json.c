#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vendor/jsmn.h"

#include "sentry_alloc.h"
#include "sentry_json.h"
#include "sentry_string.h"
#include "sentry_utils.h"
#include "sentry_value.h"

#define DST_MODE_SB 1

struct sentry_jsonwriter_s {
    union {
        sentry_stringbuilder_t *sb;
    } dst;
    uint64_t want_comma;
    uint32_t depth;
    bool last_was_key;
    char dst_mode;
};

sentry_jsonwriter_t *
sentry__jsonwriter_new_in_memory(void)
{
    sentry_jsonwriter_t *rv = SENTRY_MAKE(sentry_jsonwriter_t);
    if (!rv) {
        return NULL;
    }
    rv->dst.sb = SENTRY_MAKE(sentry_stringbuilder_t);
    if (!rv->dst.sb) {
        sentry_free(rv);
        return NULL;
    }
    sentry__stringbuilder_init(rv->dst.sb);
    rv->dst_mode = DST_MODE_SB;
    rv->want_comma = 0;
    rv->depth = 0;
    rv->last_was_key = 0;
    return rv;
}

void
sentry__jsonwriter_free(sentry_jsonwriter_t *jw)
{
    if (!jw) {
        return;
    }
    switch (jw->dst_mode) {
    case DST_MODE_SB:
        sentry__stringbuilder_cleanup(jw->dst.sb);
        sentry_free(jw->dst.sb);
        break;
    }
    sentry_free(jw);
}

size_t
sentry__jsonwriter_get_length(const sentry_jsonwriter_t *jw)
{
    switch (jw->dst_mode) {
    case DST_MODE_SB: {
        const sentry_stringbuilder_t *sb = jw->dst.sb;
        return sb->len;
    }
    default:
        return 0;
    }
}

char *
sentry__jsonwriter_into_string(sentry_jsonwriter_t *jw, size_t *len_out)
{
    char *rv = NULL;
    switch (jw->dst_mode) {
    case DST_MODE_SB: {
        sentry_stringbuilder_t *sb = jw->dst.sb;
        if (len_out) {
            *len_out = sb->len;
        }
        rv = sentry__stringbuilder_into_string(sb);
        break;
    }
    }
    sentry__jsonwriter_free(jw);
    return rv;
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
    switch (jw->dst_mode) {
    case DST_MODE_SB:
        sentry__stringbuilder_append_char(jw->dst.sb, c);
    }
}

static void
write_str(sentry_jsonwriter_t *jw, const char *str)
{
    switch (jw->dst_mode) {
    case DST_MODE_SB:
        sentry__stringbuilder_append(jw->dst.sb, str);
    }
}

static void
write_json_str(sentry_jsonwriter_t *jw, const char *str)
{
    // using unsigned here because utf-8 is > 127 :-)
    const unsigned char *ptr = (const unsigned char *)str;
    write_char(jw, '"');
    for (; *ptr; ptr++) {
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
                write_char(jw, *ptr);
            }
        }
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
        char buf[50];
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
sentry__jsonwriter_write_msec_timestamp(sentry_jsonwriter_t *jw, uint64_t time)
{
    char *formatted = sentry__msec_time_to_iso8601(time);
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
    if (!can_write_item(jw)) {
        return;
    }
    write_char(jw, '[');
    jw->depth += 1;
    set_comma(jw, false);
}

void
sentry__jsonwriter_write_list_end(sentry_jsonwriter_t *jw)
{
    write_char(jw, ']');
    jw->depth -= 1;
}

void
sentry__jsonwriter_write_object_start(sentry_jsonwriter_t *jw)
{
    if (!can_write_item(jw)) {
        return;
    }
    write_char(jw, '{');
    jw->depth += 1;
    set_comma(jw, false);
}

void
sentry__jsonwriter_write_object_end(sentry_jsonwriter_t *jw)
{
    write_char(jw, '}');
    jw->depth -= 1;
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
                output += sentry__unichar_to_utf8(uchar, output);
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
            if (val == (double)(int32_t)val) {
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
        char *string
            = sentry__string_clonen(buf + root->start, root->end - root->start);
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

            char *key = sentry__string_clonen(
                buf + token->start, token->end - token->start);
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

    jsmntok_t *tokens = sentry_malloc(sizeof(jsmntok_t) * token_count);
    jsmn_init(&jsmn_p);
    token_count = jsmn_parse(&jsmn_p, buf, buflen, tokens, token_count);

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
