#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_utils.h"

#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef SENTRY_PLATFORM_DARWIN
#    include <mach-o/dyld.h>
#endif

// only read this many bytes to memory ever
static const size_t MAX_READ_TO_BUFFER = 134217728;

struct sentry_pathiter_s {
    const sentry_path_t *parent;
    sentry_path_t *current;
    DIR *dir_handle;
};

static size_t
write_loop(int fd, const char *buf, size_t buf_len)
{
    while (buf_len > 0) {
        ssize_t n = write(fd, buf, buf_len);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        } else if (n <= 0) {
            break;
        }
        buf += n;
        buf_len -= n;
    }

    return buf_len;
}

bool
sentry__filelock_try_lock(sentry_filelock_t *lock)
{
    lock->is_locked = false;

    int fd = open(lock->path->path, O_RDONLY | O_CREAT | O_TRUNC,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (fd < 0) {
        return false;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }

    // There is possible race between the `open` and the `flock` call, in which
    // other processes could remove the file, and create a new one with the same
    // name. So we double-check *after* having the lock, that the actual file on
    // disk is actually the one we just locked. See:
    // https://stackoverflow.com/questions/17708885/flock-removing-locked-file-without-race-condition
    struct stat st0;
    struct stat st1;
    fstat(fd, &st0);
    stat(lock->path->path, &st1);
    if (st0.st_ino != st1.st_ino) {
        close(fd);
        return false;
    }

    lock->fd = fd;
    lock->is_locked = true;
    return true;
}

void
sentry__filelock_unlock(sentry_filelock_t *lock)
{
    if (!lock->is_locked) {
        return;
    }
    sentry__path_remove(lock->path);
    flock(lock->fd, LOCK_UN);
    close(lock->fd);
    lock->is_locked = false;
}

sentry_path_t *
sentry__path_absolute(const sentry_path_t *path)
{
    char full[PATH_MAX];
    if (!realpath(path->path, full)) {
        return NULL;
    }
    return sentry__path_from_str(full);
}

sentry_path_t *
sentry__path_current_exe(void)
{
#ifdef SENTRY_PLATFORM_DARWIN
    // inspired by:
    // https://github.com/rust-lang/rust/blob/0176a9eef845e7421b7e2f7ef015333a41a7c027/src/libstd/sys/unix/os.rs#L339-L358
    uint32_t buf_size = 0;
    _NSGetExecutablePath(NULL, &buf_size);
    char *buf = sentry_malloc(buf_size);
    if (!buf) {
        return NULL;
    }
    int err = _NSGetExecutablePath(buf, &buf_size);
    if (err) {
        sentry_free(buf);
        return NULL;
    }
    return sentry__path_from_str_owned(buf);
#elif defined(SENTRY_PLATFORM_LINUX)
    // inspired by:
    // https://github.com/rust-lang/rust/blob/0176a9eef845e7421b7e2f7ef015333a41a7c027/src/libstd/sys/unix/os.rs#L328-L337
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0) {
        return NULL;
    }
    buf[len] = 0;
    return sentry__path_from_str(buf);
#endif
    return NULL;
}

sentry_path_t *
sentry__path_dir(const sentry_path_t *path)
{
    char *buf = sentry__string_clone(path->path);
    if (!buf) {
        return NULL;
    }
    // dirname can modify its argument, and may return pointers to static memory
    // that we are not allowed to free.
    char *dir = dirname(buf);
    char *newpathbuf = sentry__string_clone(dir);
    sentry_free(buf);
    if (!newpathbuf) {
        return NULL;
    }
    return sentry__path_from_str_owned(newpathbuf);
}

sentry_path_t *
sentry__path_from_str(const char *s)
{
    char *path = sentry__string_clone(s);
    if (!path) {
        return NULL;
    }
    // NOTE: function will free `path` on error
    return sentry__path_from_str_owned(path);
}

sentry_path_t *
sentry__path_from_str_owned(char *s)
{
    sentry_path_t *rv = SENTRY_MAKE(sentry_path_t);
    if (!rv) {
        sentry_free(s);
        return NULL;
    }
    rv->path = s;
    return rv;
}

const sentry_pathchar_t *
sentry__path_filename(const sentry_path_t *path)
{
    const char *c = strrchr(path->path, '/');
    return c ? c + 1 : path->path;
}

bool
sentry__path_filename_matches(const sentry_path_t *path, const char *filename)
{
    return sentry__string_eq(sentry__path_filename(path), filename);
}

bool
sentry__path_ends_with(const sentry_path_t *path, const char *suffix)
{
    size_t pathlen = strlen(path->path);
    size_t suffixlen = strlen(suffix);
    if (suffixlen > pathlen) {
        return false;
    }
    return sentry__string_eq(&path->path[pathlen - suffixlen], suffix);
}

bool
sentry__path_is_dir(const sentry_path_t *path)
{
    struct stat buf;
    return stat(path->path, &buf) == 0 && S_ISDIR(buf.st_mode);
}

bool
sentry__path_is_file(const sentry_path_t *path)
{
    struct stat buf;
    return stat(path->path, &buf) == 0 && S_ISREG(buf.st_mode);
}

size_t
sentry__path_get_size(const sentry_path_t *path)
{
    struct stat buf;
    if (stat(path->path, &buf) == 0 && S_ISREG(buf.st_mode)) {
        return (size_t)buf.st_size;
    } else {
        return 0;
    }
}

sentry_path_t *
sentry__path_append_str(const sentry_path_t *base, const char *suffix)
{
    sentry_stringbuilder_t sb;

    sentry__stringbuilder_init(&sb);
    sentry__stringbuilder_append(&sb, base->path);
    sentry__stringbuilder_append(&sb, suffix);

    return sentry__path_from_str_owned(sentry__stringbuilder_into_string(&sb));
}

sentry_path_t *
sentry__path_join_str(const sentry_path_t *base, const char *other)
{
    sentry_stringbuilder_t sb;

    if (*other == '/') {
        return sentry__path_from_str(other);
    }

    sentry__stringbuilder_init(&sb);
    sentry__stringbuilder_append(&sb, base->path);

    if (!base->path[0] || base->path[strlen(base->path) - 1] != '/') {
        sentry__stringbuilder_append_char(&sb, '/');
    }
    sentry__stringbuilder_append(&sb, other);

    return sentry__path_from_str_owned(sentry__stringbuilder_into_string(&sb));
}

sentry_path_t *
sentry__path_clone(const sentry_path_t *path)
{
    sentry_path_t *rv = SENTRY_MAKE(sentry_path_t);
    if (!rv) {
        return NULL;
    }
    rv->path = sentry__string_clone(path->path);
    return rv;
}

#define EINTR_RETRY(X, Y)                                                      \
    do {                                                                       \
        int _tmp;                                                              \
        do {                                                                   \
            _tmp = (X);                                                        \
        } while (_tmp == -1 && errno == EINTR);                                \
        if (Y != 0) {                                                          \
            *(int *)Y = _tmp;                                                  \
        }                                                                      \
    } while (false)

int
sentry__path_remove(const sentry_path_t *path)
{
    int status;
    if (!sentry__path_is_dir(path)) {
        EINTR_RETRY(unlink(path->path), &status);
        if (status == 0) {
            return 0;
        }
    } else {
        EINTR_RETRY(rmdir(path->path), &status);
        if (status == 0) {
            return 0;
        }
    }
    if (errno == ENOENT) {
        return 0;
    }
    return 1;
}

int
sentry__path_create_dir_all(const sentry_path_t *path)
{
    char *p, *ptr;
    int rv = 0;
#define _TRY_MAKE_DIR                                                          \
    do {                                                                       \
        int mrv = mkdir(p, 0700);                                              \
        if (mrv != 0 && errno != EEXIST) {                                     \
            rv = 1;                                                            \
            goto done;                                                         \
        }                                                                      \
    } while (0)

    p = sentry__string_clone(path->path);
    for (ptr = p; *ptr; ptr++) {
        if (*ptr == '/' && ptr != p) {
            *ptr = 0;
            _TRY_MAKE_DIR;
            *ptr = '/';
        }
    }
    _TRY_MAKE_DIR;
#undef _TRY_MAKE_DIR

done:
    sentry_free(p);
    return rv;
}

sentry_pathiter_t *
sentry__path_iter_directory(const sentry_path_t *path)
{
    sentry_pathiter_t *rv = SENTRY_MAKE(sentry_pathiter_t);
    if (!rv) {
        return NULL;
    }
    rv->parent = path;
    rv->current = NULL;
    rv->dir_handle = opendir(path->path);
    return rv;
}

const sentry_path_t *
sentry__pathiter_next(sentry_pathiter_t *piter)
{
    struct dirent *entry;

    if (!piter->dir_handle) {
        return NULL;
    }

    while (true) {
        entry = readdir(piter->dir_handle);
        if (!entry) {
            return NULL;
        }
        if (sentry__string_eq(entry->d_name, ".")
            || sentry__string_eq(entry->d_name, "..")) {
            continue;
        }
        break;
    }

    sentry__path_free(piter->current);
    piter->current = sentry__path_join_str(piter->parent, entry->d_name);

    return piter->current;
}

void
sentry__pathiter_free(sentry_pathiter_t *piter)
{
    if (!piter) {
        return;
    }
    if (piter->dir_handle) {
        closedir(piter->dir_handle);
    }
    sentry__path_free(piter->current);
    sentry_free(piter);
}

int
sentry__path_touch(const sentry_path_t *path)
{
    int fd = open(path->path, O_WRONLY | O_CREAT | O_APPEND,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    if (fd < 0) {
        return 1;
    } else {
        close(fd);
        return 0;
    }
}

char *
sentry__path_read_to_buffer(const sentry_path_t *path, size_t *size_out)
{
    int fd = open(path->path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    size_t len = sentry__path_get_size(path);
    if (len == 0) {
        close(fd);
        char *rv = sentry_malloc(1);
        rv[0] = '\0';
        if (size_out) {
            *size_out = 0;
        }
        return rv;
    } else if (len > MAX_READ_TO_BUFFER) {
        close(fd);
        return NULL;
    }

    // this is completely not sane in concurrent situations but hey
    char *rv = sentry_malloc(len + 1);
    if (!rv) {
        close(fd);
        return NULL;
    }

    size_t remaining = len;
    size_t offset = 0;
    while (remaining > 0) {
        ssize_t n = read(fd, rv + offset, remaining);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        } else if (n <= 0) {
            break;
        }
        offset += n;
        remaining -= n;
    }

    rv[offset] = '\0';
    close(fd);

    if (size_out) {
        *size_out = offset;
    }
    return rv;
}

static int
write_buffer_with_flags(
    const sentry_path_t *path, const char *buf, size_t buf_len, int flags)
{
    int fd = open(
        path->path, flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    if (fd < 0) {
        SENTRY_TRACEF("failed to open file \"%s\" for writing", path->path);
        return 1;
    }

    size_t remaining = write_loop(fd, buf, buf_len);

    close(fd);
    return remaining == 0 ? 0 : 1;
}

int
sentry__path_write_buffer(
    const sentry_path_t *path, const char *buf, size_t buf_len)
{
    return write_buffer_with_flags(
        path, buf, buf_len, O_RDWR | O_CREAT | O_TRUNC);
}

int
sentry__path_append_buffer(
    const sentry_path_t *path, const char *buf, size_t buf_len)
{
    return write_buffer_with_flags(
        path, buf, buf_len, O_RDWR | O_CREAT | O_APPEND);
}
