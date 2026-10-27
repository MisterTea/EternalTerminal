#include "sentry_boot.h"

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)         \
    || defined(SENTRY_PLATFORM_MACOS)

#    include "sentry_alloc.h"
#    include "sentry_logger.h"
#    include "sentry_minidump_common.h"
#    include "sentry_minidump_format.h"

#    include <errno.h>
#    include <string.h>
#    include <time.h>
#    include <unistd.h>

minidump_rva_t
sentry__minidump_write_data(
    minidump_writer_base_t *writer, const void *data, size_t size)
{
    // Check for 4GB overflow before truncating to uint32_t RVA
    if (writer->current_offset > UINT32_MAX) {
        SENTRY_WARN("minidump exceeded 4GB, RVA would overflow");
        return 0;
    }
    minidump_rva_t rva = (minidump_rva_t)writer->current_offset;

    ssize_t written = write(writer->fd, data, size);
    if (written != (ssize_t)size) {
        SENTRY_WARNF("minidump write failed: %s", strerror(errno));
        // Seek back to undo any partial write so the fd position stays
        // in sync with current_offset.
        if (written > 0) {
            lseek(writer->fd, -written, SEEK_CUR);
        }
        return 0;
    }

    writer->current_offset += size;

    // Align to 4-byte boundary
    uint32_t padding = (4 - (writer->current_offset % 4)) % 4;
    if (padding > 0) {
        const uint8_t zeros[4] = { 0 };
        ssize_t pad_written = write(writer->fd, zeros, padding);
        if (pad_written == (ssize_t)padding) {
            writer->current_offset += padding;
        } else if (pad_written > 0) {
            // Seek back to undo partial padding write so fd stays in sync
            lseek(writer->fd, -pad_written, SEEK_CUR);
        }
    }

    return rva;
}

int
sentry__minidump_write_header(
    minidump_writer_base_t *writer, uint32_t stream_count)
{
    minidump_header_t header = {
        .signature = MINIDUMP_SIGNATURE,
        .version = MINIDUMP_VERSION,
        .stream_count = stream_count,
        .stream_directory_rva = sizeof(minidump_header_t),
        .checksum = 0,
        .time_date_stamp = (uint32_t)time(NULL),
        .flags = 0,
    };

    // Write directly instead of via write_data to avoid corrupting
    // current_offset. This function is called after seeking back to
    // position 0, when current_offset holds the end-of-streams value.
    ssize_t written = write(writer->fd, &header, sizeof(header));
    if (written != (ssize_t)sizeof(header)) {
        SENTRY_WARNF("minidump header write failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * Decode a UTF-8 sequence and return the Unicode code point.
 * Advances *src past the decoded bytes.
 * Returns the code point, or 0xFFFD (replacement char) on invalid input.
 */
static uint32_t
decode_utf8(const uint8_t **src, const uint8_t *end)
{
    const uint8_t *p = *src;
    if (p >= end) {
        return 0xFFFD;
    }

    uint8_t c = *p++;
    uint32_t cp;
    int extra_bytes;

    if (c < 0x80) {
        // ASCII: 0xxxxxxx
        *src = p;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        // 2-byte: 110xxxxx 10xxxxxx
        cp = c & 0x1F;
        extra_bytes = 1;
    } else if ((c & 0xF0) == 0xE0) {
        // 3-byte: 1110xxxx 10xxxxxx 10xxxxxx
        cp = c & 0x0F;
        extra_bytes = 2;
    } else if ((c & 0xF8) == 0xF0) {
        // 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        cp = c & 0x07;
        extra_bytes = 3;
    } else {
        // Invalid leading byte - skip it
        *src = p;
        return 0xFFFD;
    }

    // Read continuation bytes
    for (int i = 0; i < extra_bytes; i++) {
        if (p >= end || (*p & 0xC0) != 0x80) {
            // Missing or invalid continuation byte
            *src = p;
            return 0xFFFD;
        }
        cp = (cp << 6) | (*p++ & 0x3F);
    }

    *src = p;

    // Validate: reject overlong encodings and surrogates
    if ((extra_bytes == 1 && cp < 0x80) || (extra_bytes == 2 && cp < 0x800)
        || (extra_bytes == 3 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF)
        || cp > 0x10FFFF) {
        return 0xFFFD;
    }

    return cp;
}

minidump_rva_t
sentry__minidump_write_string(
    minidump_writer_base_t *writer, const char *utf8_str)
{
    if (!utf8_str) {
        return 0;
    }

    size_t utf8_len = strlen(utf8_str);

    // Sanity check: prevent integer overflow and reject unreasonably long
    // strings. Max reasonable module name/path is ~32KB, which fits in uint32_t
    if (utf8_len > 32768) {
        SENTRY_WARNF("minidump string too long: %zu bytes", utf8_len);
        return 0;
    }

    // Allocate buffer for: length (4 bytes) + UTF-16LE chars + null terminator
    // In the worst case, each UTF-8 byte could become one UTF-16 code unit
    // (ASCII), and code points > U+FFFF need surrogate pairs (2 code units).
    // Using utf8_len code units is always sufficient.
    size_t max_utf16_units = utf8_len;
    size_t buf_size
        = sizeof(uint32_t) + (max_utf16_units * 2) + 2; // +2 for null
    uint8_t *buf = sentry_malloc(buf_size);
    if (!buf) {
        return 0;
    }

    // Convert UTF-8 to UTF-16LE
    uint16_t *utf16 = (uint16_t *)(buf + sizeof(uint32_t));
    const uint8_t *src = (const uint8_t *)utf8_str;
    const uint8_t *end = src + utf8_len;
    size_t utf16_count = 0;

    while (src < end) {
        uint32_t cp = decode_utf8(&src, end);

        if (cp <= 0xFFFF) {
            // BMP character - single UTF-16 code unit
            utf16[utf16_count++] = (uint16_t)cp;
        } else {
            // Supplementary character - surrogate pair
            cp -= 0x10000;
            utf16[utf16_count++] = (uint16_t)(0xD800 | (cp >> 10));
            utf16[utf16_count++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        }
    }
    utf16[utf16_count] = 0; // Null terminator

    // Write string length in bytes (NOT including null terminator)
    uint32_t string_bytes = (uint32_t)(utf16_count * 2);
    memcpy(buf, &string_bytes, sizeof(uint32_t));

    // Calculate actual size used
    size_t total_size = sizeof(uint32_t) + (utf16_count * 2) + 2;

    minidump_rva_t rva = sentry__minidump_write_data(writer, buf, total_size);
    sentry_free(buf);
    return rva;
}

size_t
sentry__minidump_get_context_size(void)
{
#    if defined(__x86_64__)
    return sizeof(minidump_context_x86_64_t);
#    elif defined(__aarch64__)
    return sizeof(minidump_context_arm64_t);
#    elif defined(__i386__)
    return sizeof(minidump_context_x86_t);
#    elif defined(__arm__)
    return sizeof(minidump_context_arm_t);
#    else
#        error "Unsupported architecture"
#    endif
}

#endif // SENTRY_PLATFORM_LINUX || SENTRY_PLATFORM_ANDROID ||
       // SENTRY_PLATFORM_MACOS
