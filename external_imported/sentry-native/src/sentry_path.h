#ifndef SENTRY_PATH_H_INCLUDED
#define SENTRY_PATH_H_INCLUDED

#include "sentry_boot.h"

#include <stdio.h>

#ifdef SENTRY_PLATFORM_WINDOWS
typedef wchar_t sentry_pathchar_t;
#    define SENTRY_PATH_PRI "S"
#else
typedef char sentry_pathchar_t;
#    define SENTRY_PATH_PRI "s"
#endif

struct sentry_path_s {
    sentry_pathchar_t *path;
};

struct sentry_filelock_s {
    struct sentry_path_s *path;
    int fd;
    bool is_locked;
};

typedef struct sentry_path_s sentry_path_t;
typedef struct sentry_pathiter_s sentry_pathiter_t;
typedef struct sentry_filelock_s sentry_filelock_t;

/**
 * NOTE on encodings:
 *
 * When not stated otherwise, all `char` functions defined here will assume an
 * OS-specific encoding, typically ANSI on Windows, and UTF-8 on Unix.
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
 * Return a new path with the given suffix appended.
 * This is different to `sentry__path_join_str` as it does not create a new path
 * segment.
 */
sentry_path_t *sentry__path_append_str(
    const sentry_path_t *base, const char *suffix);

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
const sentry_pathchar_t *sentry__path_filename(const sentry_path_t *path);

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
 * Return whether the path refers to a regular file.
 */
bool sentry__path_is_file(const sentry_path_t *path);

/**
 * Remove the directory or file referred to by `path`.
 * This will *not* recursively delete any directory content. Use
 * `sentry__path_remove_all` for that.
 * Returns 0 on success.
 */
int sentry__path_remove(const sentry_path_t *path);

/**
 * Recursively remove the given directory and everything in it.
 * Returns 0 on success.
 */
int sentry__path_remove_all(const sentry_path_t *path);

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
 * This will read all the content of `path` into a newly allocated buffer, and
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
 * The function will return `false` when no lock can be acquired, for example if
 * the lock is being held by another process.
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

/* windows specific API additions */
#ifdef SENTRY_PLATFORM_WINDOWS
/**
 * Create a new path from a Wide String.
 */
sentry_path_t *sentry__path_from_wstr(const wchar_t *s);

/**
 * Create another path by appending a new path segment.
 */
sentry_path_t *sentry__path_join_wstr(
    const sentry_path_t *base, const wchar_t *other);
#endif

/**
 * Create a new path from the platform native string type.
 */
static inline sentry_path_t *
sentry__path_new(const sentry_pathchar_t *s)
{
#ifdef SENTRY_PLATFORM_WINDOWS
    return sentry__path_from_wstr(s);
#else
    return sentry__path_from_str(s);
#endif
}

#endif
