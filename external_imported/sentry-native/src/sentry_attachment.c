#include "sentry_attachment.h"
#include "sentry_alloc.h"
#include "sentry_logger.h"
#include "sentry_options.h"
#include "sentry_path.h"
#include "sentry_string.h"

#include <string.h>

void
sentry_attachment_set_type(sentry_attachment_t *attachment, const char *type)
{
    sentry_attachment_set_type_n(
        attachment, type, sentry__guarded_strlen(type));
}

void
sentry_attachment_set_type_n(
    sentry_attachment_t *attachment, const char *type, size_t type_len)
{
    if (!attachment) {
        return;
    }

    sentry_free(attachment->type);
    attachment->type
        = type && type_len > 0 ? sentry__string_clone_n(type, type_len) : NULL;

    if (attachment->type && !attachment->content_type) {
        if (sentry__string_eq(
                attachment->type, SENTRY_ATTACHMENT_TYPE_MINIDUMP)) {
            attachment->content_type
                = sentry__string_clone("application/octet-stream");
        } else if (sentry__string_eq(attachment->type,
                       SENTRY_ATTACHMENT_TYPE_VIEW_HIERARCHY)) {
            attachment->content_type = sentry__string_clone("application/json");
        }
    }
}

void
sentry_attachment_set_content_type(
    sentry_attachment_t *attachment, const char *content_type)
{
    sentry_attachment_set_content_type_n(
        attachment, content_type, sentry__guarded_strlen(content_type));
}

void
sentry_attachment_set_content_type_n(sentry_attachment_t *attachment,
    const char *content_type, size_t content_type_len)
{
    if (!attachment) {
        return;
    }

    sentry_free(attachment->content_type);
    attachment->content_type
        = sentry__string_clone_n(content_type, content_type_len);
}

void
sentry_attachment_set_filename(
    sentry_attachment_t *attachment, const char *filename)
{
    sentry_attachment_set_filename_n(
        attachment, filename, sentry__guarded_strlen(filename));
}

void
sentry_attachment_set_filename_n(
    sentry_attachment_t *attachment, const char *filename, size_t filename_len)
{
    if (!attachment) {
        return;
    }

    sentry__path_free(attachment->filename);
    attachment->filename = sentry__path_from_str_n(filename, filename_len);
}

#ifdef SENTRY_PLATFORM_WINDOWS
void
sentry_attachment_set_filenamew(
    sentry_attachment_t *attachment, const wchar_t *filename)
{
    size_t filename_len = filename ? wcslen(filename) : 0;
    sentry_attachment_set_filenamew_n(attachment, filename, filename_len);
}

void
sentry_attachment_set_filenamew_n(sentry_attachment_t *attachment,
    const wchar_t *filename, size_t filename_len)
{
    if (!attachment) {
        return;
    }

    sentry__path_free(attachment->filename);
    attachment->filename = sentry__path_from_wstr_n(filename, filename_len);
}
#endif

sentry_attachment_t *
sentry__attachment_from_path(sentry_path_t *path)
{
    if (!path) {
        return NULL;
    }
    sentry_attachment_t *attachment = SENTRY_MAKE(sentry_attachment_t);
    if (!attachment) {
        sentry__path_free(path);
        return NULL;
    }
    attachment->path = path;
    return attachment;
}

sentry_attachment_t *
sentry__attachment_from_buffer(
    const char *buf, size_t buf_len, sentry_path_t *filename)
{
    if (!filename) {
        return NULL;
    }
    if (!buf || !buf_len) {
        sentry__path_free(filename);
        return NULL;
    }
    sentry_attachment_t *attachment = SENTRY_MAKE(sentry_attachment_t);
    if (!attachment) {
        sentry__path_free(filename);
        return NULL;
    }
    attachment->filename = filename;
    attachment->buf = sentry_malloc(buf_len * sizeof(char));
    memcpy(attachment->buf, buf, buf_len * sizeof(char));
    attachment->buf_len = buf_len;
    return attachment;
}

void
sentry__attachment_free(sentry_attachment_t *attachment)
{
    if (!attachment) {
        return;
    }
    sentry__path_free(attachment->path);
    sentry__path_free(attachment->filename);
    sentry_free(attachment->buf);
    sentry_free(attachment->type);
    sentry_free(attachment->content_type);
    sentry_free(attachment);
}

size_t
sentry__attachment_get_size(const sentry_attachment_t *attachment)
{
    return attachment->buf ? attachment->buf_len
                           : sentry__path_get_size(attachment->path);
}

const char *
sentry__attachment_get_filename(const sentry_attachment_t *attachment)
{
    const sentry_path_t *path
        = attachment->filename ? attachment->filename : attachment->path;
    return path ? sentry__path_filename(path) : NULL;
}

bool
sentry__attachment_is_placeholder(
    const sentry_attachment_t *att, const sentry_options_t *options)
{
    return options && options->enable_large_attachments && att
        && sentry__attachment_get_size(att) >= SENTRY_LARGE_ATTACHMENT_SIZE;
}

void
sentry__attachments_free(sentry_attachment_t *attachments)
{
    sentry_attachment_t *it = attachments;
    while (it) {
        sentry_attachment_t *attachment = it;
        it = attachment->next;

        sentry__attachment_free(attachment);
    }
}

/**
 * Compares attachments for equality to avoid adding duplicates.
 *
 * File attachments are considered equal if the paths are equal. Byte buffer
 * attachments are only considered equal to themselves - they are not required
 * to have unique filenames, and the bytes are not compared.
 */
static bool
attachment_eq(const sentry_attachment_t *a, const sentry_attachment_t *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b || a->buf || b->buf) {
        return false;
    }
    const char *a_type = a->type ? a->type : "";
    const char *b_type = b->type ? b->type : "";
    if (!sentry__string_eq(a_type, b_type)) {
        return false;
    }
    return sentry__path_eq(a->path, b->path);
}

sentry_attachment_t *
sentry__attachments_add(
    sentry_attachment_t **attachments_ptr, sentry_attachment_t *attachment)
{
    if (!attachment) {
        return NULL;
    }
    size_t size = sentry__attachment_get_size(attachment);
    if (size > SENTRY_MAX_ATTACHMENT_SIZE) {
        SENTRY_WARNF("rejected oversized attachment \"%s\" (%zu > %d MiB)",
            sentry__attachment_get_filename(attachment), size / (1024 * 1024),
            SENTRY_MAX_ATTACHMENT_SIZE / (1024 * 1024));
        sentry__attachment_free(attachment);
        return NULL;
    }
    if (size >= SENTRY_LARGE_ATTACHMENT_SIZE) {
        SENTRY_INFOF("added large attachment \"%s\" (%zu MiB)",
            sentry__attachment_get_filename(attachment), size / (1024 * 1024));
    }

    sentry_attachment_t **next_ptr = attachments_ptr;

    for (sentry_attachment_t *it = *attachments_ptr; it; it = it->next) {
        if (attachment_eq(it, attachment)) {
            sentry__attachment_free(attachment);
            return it;
        }

        next_ptr = &it->next;
    }

    *next_ptr = attachment;
    return attachment;
}

sentry_attachment_t *
sentry__attachments_add_path(sentry_attachment_t **attachments_ptr,
    sentry_path_t *path, const char *attachment_type, const char *content_type)
{
    sentry_attachment_t *attachment = sentry__attachment_from_path(path);
    if (!attachment) {
        return NULL;
    }
    sentry_attachment_set_type(attachment, attachment_type);
    if (content_type || !attachment->content_type) {
        sentry_attachment_set_content_type(attachment, content_type);
    }
    return sentry__attachments_add(attachments_ptr, attachment);
}

void
sentry__attachments_remove(
    sentry_attachment_t **attachments_ptr, sentry_attachment_t *attachment)
{
    if (!attachment) {
        return;
    }

    sentry_attachment_t **next_ptr = attachments_ptr;

    for (sentry_attachment_t *it = *attachments_ptr; it; it = it->next) {
        if (it == attachment) {
            *next_ptr = it->next;
            sentry__attachment_free(it);
            return;
        }

        next_ptr = &it->next;
    }
}

static sentry_attachment_t *
attachment_clone(const sentry_attachment_t *attachment)
{
    if (!attachment) {
        return NULL;
    }

    sentry_attachment_t *clone = SENTRY_MAKE(sentry_attachment_t);
    if (!clone) {
        return NULL;
    }

    if (attachment->path) {
        clone->path = sentry__path_clone(attachment->path);
        if (!clone->path) {
            goto fail;
        }
    }
    if (attachment->filename) {
        clone->filename = sentry__path_clone(attachment->filename);
        if (!clone->filename) {
            goto fail;
        }
    }
    if (attachment->buf) {
        clone->buf_len = attachment->buf_len;
        clone->buf = sentry_malloc(attachment->buf_len * sizeof(char));
        if (!clone->buf) {
            goto fail;
        }
        memcpy(clone->buf, attachment->buf, attachment->buf_len * sizeof(char));
    }
    if (attachment->type) {
        clone->type = sentry__string_clone(attachment->type);
        if (!clone->type) {
            goto fail;
        }
    }
    if (attachment->content_type) {
        clone->content_type = sentry__string_clone(attachment->content_type);
        if (!clone->content_type) {
            goto fail;
        }
    }
    return clone;

fail:
    sentry__attachment_free(clone);
    return NULL;
}

void
sentry__attachments_extend(
    sentry_attachment_t **attachments_ptr, sentry_attachment_t *attachments)
{
    for (sentry_attachment_t *it = attachments; it; it = it->next) {
        sentry__attachments_add(attachments_ptr, attachment_clone(it));
    }
}
