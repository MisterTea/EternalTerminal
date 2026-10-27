#include "sentry_writer.h"
#include "sentry_alloc.h"
#include "sentry_core.h"

typedef struct sentry_writer_ops_s {
    size_t (*write)(void *state, const char *buf, size_t len);
    bool (*close)(void *state);
    void (*free)(void *state);
    char *(*into_string)(void *state, bool failed, size_t *len_out);
} sentry_writer_ops_t;

struct sentry_writer_s {
    void *state;
    const sentry_writer_ops_t *ops;
    size_t byte_count;
    bool failed;
    bool closed;
};

typedef struct sb_writer_state_s {
    sentry_stringbuilder_t *sb;
    bool owns_sb;
} sb_writer_state_t;

static size_t
sb_writer_write(void *state, const char *buf, size_t len)
{
    sb_writer_state_t *sb_state = state;
    return sentry__stringbuilder_append_buf(sb_state->sb, buf, len) == 0 ? 0
                                                                         : len;
}

static bool
sb_writer_close(void *UNUSED(state))
{
    return true;
}

static void
sb_writer_free(void *state)
{
    sb_writer_state_t *sb_state = state;
    if (!sb_state) {
        return;
    }
    if (sb_state->owns_sb) {
        sentry__stringbuilder_cleanup(sb_state->sb);
        sentry_free(sb_state->sb);
    }
    sentry_free(sb_state);
}

static char *
sb_writer_into_string(void *state, bool failed, size_t *len_out)
{
    sb_writer_state_t *sb_state = state;
    char *rv = NULL;

    if (len_out) {
        *len_out = failed ? 0 : sentry__stringbuilder_len(sb_state->sb);
    }

    if (!failed) {
        // `sentry__writer_new_sb` accepts both owned and caller-owned string
        // builders. In both cases, converting into a string means detaching the
        // builder buffer. If the builder itself is caller-owned, only the
        // buffer is consumed and the empty builder is left with the caller.
        rv = sentry_stringbuilder_take_string(sb_state->sb);
    }

    sb_writer_free(state);
    return rv;
}

static const sentry_writer_ops_t sb_writer_ops = {
    .write = sb_writer_write,
    .close = sb_writer_close,
    .free = sb_writer_free,
    .into_string = sb_writer_into_string,
};

typedef struct file_writer_state_s {
    sentry_filewriter_t *fw;
    bool close_on_free;
} file_writer_state_t;

static size_t
file_writer_write(void *state, const char *buf, size_t len)
{
    file_writer_state_t *file_state = state;
    return sentry__filewriter_write(file_state->fw, buf, len);
}

static bool
file_writer_close(void *state)
{
    file_writer_state_t *file_state = state;
    if (!file_state->close_on_free) {
        return !sentry__filewriter_has_failed(file_state->fw);
    }
    return sentry__filewriter_close(file_state->fw);
}

static void
file_writer_free(void *state)
{
    file_writer_state_t *file_state = state;
    if (!file_state) {
        return;
    }
    if (file_state->close_on_free) {
        sentry__filewriter_free(file_state->fw);
    }
    sentry_free(file_state);
}

static char *
file_writer_into_string(void *state, bool UNUSED(failed), size_t *len_out)
{
    // `sentry__writer_into_string` is defined for all writer kinds and
    // according to the module header must return NULL for non-string sinks.
    if (len_out) {
        *len_out = 0;
    }
    file_writer_free(state);
    return NULL;
}

static const sentry_writer_ops_t file_writer_ops = {
    .write = file_writer_write,
    .close = file_writer_close,
    .free = file_writer_free,
    .into_string = file_writer_into_string,
};

static sentry_writer_t *
writer_new(void *state, const sentry_writer_ops_t *ops)
{
    sentry_writer_t *writer = SENTRY_MAKE(sentry_writer_t);
    if (!writer) {
        ops->free(state);
        return NULL;
    }

    writer->state = state;
    writer->ops = ops;
    writer->byte_count = 0;
    writer->failed = false;
    writer->closed = false;
    return writer;
}

sentry_writer_t *
sentry__writer_new_sb(sentry_stringbuilder_t *sb)
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

    sb_writer_state_t *state = SENTRY_MAKE(sb_writer_state_t);
    if (!state) {
        if (owns_sb) {
            sentry__stringbuilder_cleanup(sb);
            sentry_free(sb);
        }
        return NULL;
    }
    state->sb = sb;
    state->owns_sb = owns_sb;

    return writer_new(state, &sb_writer_ops);
}

sentry_writer_t *
sentry__writer_new_filewriter(sentry_filewriter_t *fw, bool close_on_free)
{
    if (!fw) {
        return NULL;
    }

    file_writer_state_t *state = SENTRY_MAKE(file_writer_state_t);
    if (!state) {
        if (close_on_free) {
            sentry__filewriter_free(fw);
        }
        return NULL;
    }
    state->fw = fw;
    state->close_on_free = close_on_free;

    return writer_new(state, &file_writer_ops);
}

sentry_writer_t *
sentry__writer_new_file(const sentry_path_t *path)
{
    sentry_filewriter_t *fw = sentry__filewriter_new(path);
    if (!fw) {
        return NULL;
    }
    return sentry__writer_new_filewriter(fw, true);
}

void
sentry__writer_free(sentry_writer_t *writer)
{
    if (!writer) {
        return;
    }

    // Freeing is also a finalization point. This protects callers that forget
    // to close explicitly, but callers that need to observe close/flush errors
    // should call `sentry__writer_close()` before freeing.
    sentry__writer_close(writer);
    writer->ops->free(writer->state);
    sentry_free(writer);
}

bool
sentry__writer_write(sentry_writer_t *writer, const char *buf, size_t len)
{
    if (!writer) {
        return false;
    }
    if (writer->failed) {
        return false;
    }
    if (writer->closed) {
        writer->failed = true;
        return false;
    }
    if (len == 0) {
        return true;
    }

    size_t remaining = writer->ops->write(writer->state, buf, len);
    writer->byte_count += len - remaining;
    if (remaining != 0) {
        writer->failed = true;
        return false;
    }
    return true;
}

bool
sentry__writer_write_char(sentry_writer_t *writer, char c)
{
    return sentry__writer_write(writer, &c, sizeof(c));
}

bool
sentry__writer_close(sentry_writer_t *writer)
{
    if (!writer) {
        return false;
    }
    if (writer->closed) {
        return !writer->failed;
    }

    writer->closed = true;
    if (!writer->ops->close(writer->state)) {
        writer->failed = true;
    }
    return !writer->failed;
}

bool
sentry__writer_has_failed(const sentry_writer_t *writer)
{
    return !writer || writer->failed;
}

size_t
sentry__writer_byte_count(const sentry_writer_t *writer)
{
    return writer ? writer->byte_count : 0;
}

char *
sentry__writer_into_string(sentry_writer_t *writer, size_t *len_out)
{
    if (!writer) {
        if (len_out) {
            *len_out = 0;
        }
        return NULL;
    }

    sentry__writer_close(writer);
    char *rv = writer->ops->into_string(
        writer->state, sentry__writer_has_failed(writer), len_out);
    sentry_free(writer);
    return rv;
}
