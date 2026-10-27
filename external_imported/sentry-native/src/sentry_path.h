#ifndef SENTRY_PATH_H_INCLUDED
#define SENTRY_PATH_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_string.h"

#include <time.h>

struct sentry_path_s {
    char *path;
#ifdef SENTRY_PLATFORM_WINDOWS
    wchar_t *path_w;
#endif
};

struct sentry_filelock_s {
    struct sentry_path_s *path;
    int fd;
    bool is_locked;
};

struct sentry_filewriter_s;

typedef struct sentry_path_s sentry_path_t;
typedef struct sentry_pathiter_s sentry_pathiter_t;
typedef struct sentry_filelock_s sentry_filelock_t;
typedef struct sentry_filewriter_s sentry_filewriter_t;

/**
 * NOTE on encodings:
 *
 * All `char` represent OS-dependent encoding. On UNIXes the path encoding is
 * based on the locale settings, which often means UTF-8 (macOS forces it,
 * Android defaults to it on all layers, and many Linux configurations default
 * to it too, but there is much more variety).
 * However, the locale can be set to anything so that we consider paths as an
 * opaque bytestream that we just pass through.
 * On Windows, we use UTF-8 as the canonical narrow string encoding and provide
 * a wide character string as an additional path member `path_w`, which all
 * functions below must keep in sync.
 *
 * If you add a new function that creates paths, you must take care of
 * synchronizing the contents of `path` and `path_w` on Windows. Further must
 * you ensure that `char` on Windows stays independent of the ANSI code page.
 *
 * In particular this means:
 * - always do full conversions between the narrow and wide characters
 * - always use the wide variant of Win32 APIs when leaving the SDK boundary
 *   (the narrow APIs interpret a `char*` according to the configured ACP)
 * - never assume you can calculate the buffer size for one encoding out of the
 *   buffer size for the other (use our string helpers or, if you must, the
 *   Win32 multibyte APIs with `CP_UTF8`!)
 */

/**
 * Creates a new path by making `path` into an absolute path.
 */
sentry_path_t *sentry__path_absolute(const sentry_path_t *path);

/**
 * This will return the path to the current executable running the code.
 */
sentry_path_t *sentry__path_current_exe(void);

/**
 * This will return the parent directory name of the given `path`.
 */
sentry_path_t *sentry__path_dir(const sentry_path_t *path);

/**
 * Create a new path from the given string.
 */
sentry_path_t *sentry__path_from_str(const char *s);
sentry_path_t *sentry__path_from_str_n(const char *s, size_t s_len);

/**
 * Create a new path from the given string.
 * The string is moved into the returned path instead of copied.
 */
sentry_path_t *sentry__path_from_str_owned(char *s);

/**
 * Return a new path with a new path segment (directory or file name) appended.
 */
sentry_path_t *sentry__path_join_str(
    const sentry_path_t *base, const char *other);

/**
 * Return a new path with the given suffix stripped.
 * If `suffix` is NULL, strips at the last dot in the filename.
 * Returns NULL if the suffix does not match.
 */
sentry_path_t *sentry__path_basename(
    const sentry_path_t *path, const char *suffix);

/**
 * Return a new path with the given suffix appended.
 * This is different to `sentry__path_join_str` as it does not create a new path
 * segment.
 */
sentry_path_t *sentry__path_append_str(
    const sentry_path_t *base, const char *suffix);

/**
 * Return a path under `dir` that does not collide with any existing file.
 * If `basename` is free, returns a path using it. Otherwise inserts `-1`, `-2`,
 * … before the extension until a free path is found.
 * Returns NULL on error or if no free name is found within a reasonable
 * number of attempts. Caller owns the returned path.
 */
sentry_path_t *sentry__path_unique(
    const sentry_path_t *dir, const char *basename);

/**
 * Creates a copy of the path.
 */
sentry_path_t *sentry__path_clone(const sentry_path_t *path);

/**
 * Free the path instance.
 */
void sentry__path_free(sentry_path_t *path);

/**
 * This will return a pointer to the last path segment, which is typically the
 * file or directory name
 */
const char *sentry__path_filename(const sentry_path_t *path);

/**
 * Returns whether the two paths are equal.
 */
bool sentry__path_eq(const sentry_path_t *path_a, const sentry_path_t *path_b);

/**
 * Returns whether the last path segment matches `filename`.
 */
bool sentry__path_filename_matches(
    const sentry_path_t *path, const char *filename);

/**
 * This will check for a specific suffix.
 */
bool sentry__path_ends_with(const sentry_path_t *path, const char *suffix);

/**
 * Return whether the path refers to a directory.
 */
bool sentry__path_is_dir(const sentry_path_t *path);

/**
 * Return whether the path refers to a symlink.
 */
bool sentry__path_is_symlink(const sentry_path_t *path);

/**
 * Return whether the path refers to a regular file.
 */
bool sentry__path_is_file(const sentry_path_t *path);

/**
 * Remove the directory or file referred to by `path`.
 * This will *not* recursively delete any directory content. Use
 * `sentry__path_remove_all` for that.
 *
 * Returns 0 on success. Success means that a file or directory was either
 * successfully removed or didn't exist before removal. Anything else is a
 * failure (i.e., return != 0).
 */
int sentry__path_remove(const sentry_path_t *path);

/**
 * Recursively remove the given directory and everything in it.
 * Returns 0 on success.
 */
int sentry__path_remove_all(const sentry_path_t *path);

/**
 * Rename/move the file or directory from `src` to `dst`.
 * This will overwrite `dst` if it already exists.
 * Returns 0 on success.
 */
int sentry__path_rename(const sentry_path_t *src, const sentry_path_t *dst);

/**
 * Copy the file from `src` to `dst`.
 * Returns 0 on success.
 */
int sentry__path_copy(const sentry_path_t *src, const sentry_path_t *dst);

/**
 * This will create the directory referred to by `path`, and any non-existing
 * parent directory.
 * Returns 0 on success.
 */
int sentry__path_create_dir_all(const sentry_path_t *path);

/**
 * This will touch or create an empty file at `path`.
 * Returns 0 on success.
 */
int sentry__path_touch(const sentry_path_t *path);

/**
 * This will return the size of the file at `path`, or 0 on failure.
 */
size_t sentry__path_get_size(const sentry_path_t *path);

/**
 * This will return the last modification time of the file at `path`, or 0 on
 * failure.
 */
time_t sentry__path_get_mtime(const sentry_path_t *path);

/**
 * This will read all the content of `path` into a newly allocated buffer and
 * write its size into `size_out`.
 */
char *sentry__path_read_to_buffer(const sentry_path_t *path, size_t *size_out);

/**
 * This will truncate the given file and write the given `buf` into it.
 */
int sentry__path_write_buffer(
    const sentry_path_t *path, const char *buf, size_t buf_len);

/**
 * This will append `buf` to an existing file.
 */
int sentry__path_append_buffer(
    const sentry_path_t *path, const char *buf, size_t buf_len);

/**
 * Create a new directory iterator for `path`.
 */
sentry_pathiter_t *sentry__path_iter_directory(const sentry_path_t *path);

/**
 * This will return a borrowed path to the next file or directory for the given
 * `piter`.
 */
const sentry_path_t *sentry__pathiter_next(sentry_pathiter_t *piter);

/**
 * This will close and free the previously created directory iterator.
 */
void sentry__pathiter_free(sentry_pathiter_t *piter);

/**
 * Create a new lockfile at the given path.
 */
sentry_filelock_t *sentry__filelock_new(sentry_path_t *path);

/**
 * This will try to acquire a lock on the given file.
 * The function will return `false` when no lock can be acquired, for example,
 * if the lock is being held by another process.
 */
bool sentry__filelock_try_lock(sentry_filelock_t *lock);

/**
 * This will release the lock on the given file.
 */
void sentry__filelock_unlock(sentry_filelock_t *lock);

/**
 * Free the allocated lockfile. This will unlock the file first.
 */
void sentry__filelock_free(sentry_filelock_t *lock);

/**
 * Create a new file-writer, which is a stateful abstraction over the
 * OS-specific file-handle and a byte counter.
 */
sentry_filewriter_t *sentry__filewriter_new(const sentry_path_t *path);

/**
 * Writes a buffer to the file behind the handle stored in the filewriter.
 * Returns the number of bytes left unwritten. A non-zero return value marks the
 * filewriter as failed and that failure is sticky.
 */
size_t sentry__filewriter_write(
    sentry_filewriter_t *filewriter, const char *buf, size_t buf_len);

/**
 * Finalizes the writer. This reports late flush/close errors and marks the
 * filewriter as failed if finalization fails. Returns true on success.
 */
bool sentry__filewriter_close(sentry_filewriter_t *filewriter);

/**
 * Returns true once any write, flush, or close operation has failed.
 */
bool sentry__filewriter_has_failed(const sentry_filewriter_t *filewriter);

/**
 * Retrieves the count of written bytes.
 */
size_t sentry__filewriter_byte_count(const sentry_filewriter_t *filewriter);

/**
 * Frees the filewriter and closes the handle.
 */
void sentry__filewriter_free(sentry_filewriter_t *filewriter);

/* windows-specific API additions */
#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Create a new path from a Wide String.
 */
sentry_path_t *sentry__path_from_wstr(const wchar_t *s);
sentry_path_t *sentry__path_from_wstr_n(const wchar_t *s, size_t s_len);

/**
 * Create another path by appending a new path segment.
 */
sentry_path_t *sentry__path_join_wstr(
    const sentry_path_t *base, const wchar_t *other);

/**
 * This will return a wide character pointer to the last path segment, which
 * is typically the file or directory name.
 */
const wchar_t *sentry__path_filename_w(const sentry_path_t *path);
#endif

/**
 * Create a new path from string.
 */
static inline sentry_path_t *
sentry__path_new(const char *s)
{
    return sentry__path_from_str(s);
}

#endif
