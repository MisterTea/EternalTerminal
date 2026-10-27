#ifndef SENTRY_WRITER_H_INCLUDED
#define SENTRY_WRITER_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_path.h"
#include "sentry_string.h"

typedef struct sentry_writer_s sentry_writer_t;

/**
 * A generic write-all byte sink used by serializers.
 *
 * The important invariant is that output errors are sticky and live at the
 * sink layer, not at every caller. A partial write, flush failure, or close
 * failure marks the writer as failed. Once failed, subsequent writes are
 * ignored and `sentry__writer_has_failed()` stays true until the writer is
 * destroyed.
 *
 * This lets higher-level serializers (such as the JSON writer or envelope
 * writer) issue a sequence of writes and check once at the end instead of
 * requiring every caller to remember to inspect every small write operation.
 */
sentry_writer_t *sentry__writer_new_sb(sentry_stringbuilder_t *sb);
sentry_writer_t *sentry__writer_new_file(const sentry_path_t *path);
sentry_writer_t *sentry__writer_new_filewriter(
    sentry_filewriter_t *fw, bool close_on_free);

void sentry__writer_free(sentry_writer_t *writer);

/**
 * Writes the full buffer or marks the writer as failed.
 * Returns true on success.
 */
bool sentry__writer_write(sentry_writer_t *writer, const char *buf, size_t len);
bool sentry__writer_write_char(sentry_writer_t *writer, char c);

/**
 * Finalizes the underlying sink. For files this includes flushing/closing, so
 * errors that are reported late by the OS are part of the writer's sticky
 * failure state. Returns true on success.
 */
bool sentry__writer_close(sentry_writer_t *writer);

bool sentry__writer_has_failed(const sentry_writer_t *writer);
size_t sentry__writer_byte_count(const sentry_writer_t *writer);

/**
 * Consumes a string-backed writer and returns its buffer. Returns NULL if the
 * writer has failed or is not string-backed.
 */
char *sentry__writer_into_string(sentry_writer_t *writer, size_t *len_out);

#endif
