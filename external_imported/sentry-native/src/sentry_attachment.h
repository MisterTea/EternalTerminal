#ifndef SENTRY_ATTACHMENT_H_INCLUDED
#define SENTRY_ATTACHMENT_H_INCLUDED

#include "sentry_boot.h"

#include "sentry_path.h"

/**
 * The attachment_type.
 */
typedef enum {
    ATTACHMENT,
    MINIDUMP,
    VIEW_HIERARCHY,
} sentry_attachment_type_t;

/**
 * This is a linked list of all the attachments registered via
 * `sentry_options_add_attachment`.
 */
struct sentry_attachment_s {
    sentry_path_t *path;
    sentry_path_t *filename;
    char *buf;
    size_t buf_len;
    sentry_attachment_type_t type;
    char *content_type;
    sentry_attachment_t *next;
};

/**
 *  Creates a new file attachment. Takes ownership of `path`.
 */
sentry_attachment_t *sentry__attachment_from_path(sentry_path_t *path);

/**
 * Creates a new byte attachment from a copy of `buf`. Takes ownership of
 * `filename`.
 */
sentry_attachment_t *sentry__attachment_from_buffer(
    const char *buf, size_t buf_len, sentry_path_t *filename);

/**
 *  Frees the `attachment`.
 */
void sentry__attachment_free(sentry_attachment_t *attachment);

/**
 * Frees the linked list of `attachments`.
 */
void sentry__attachments_free(sentry_attachment_t *attachments);

/**
 * Adds an attachment to the attachments list at `attachments_ptr`.
 */
sentry_attachment_t *sentry__attachments_add(
    sentry_attachment_t **attachments_ptr, sentry_attachment_t *attachment,
    sentry_attachment_type_t attachment_type, const char *content_type);

/**
 * Adds a file attachment to the attachments list at `attachments_ptr`.
 */
sentry_attachment_t *sentry__attachments_add_path(
    sentry_attachment_t **attachments_ptr, sentry_path_t *path,
    sentry_attachment_type_t attachment_type, const char *content_type);

/**
 * Removes an attachment from the attachments list at `attachments_ptr`.
 */
void sentry__attachments_remove(
    sentry_attachment_t **attachments_ptr, sentry_attachment_t *attachment);

/**
 * Extends the linked list of attachments at `attachments_ptr` with all
 * attachments in `attachments`.
 */
void sentry__attachments_extend(
    sentry_attachment_t **attachments_ptr, sentry_attachment_t *attachments);

#endif
