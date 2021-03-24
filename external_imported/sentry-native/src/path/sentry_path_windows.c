#include "sentry_boot.h"

#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <sys/locking.h>
#include <sys/stat.h>
#include <sys/types.h>

// only read this many bytes to memory ever
static const size_t MAX_READ_TO_BUFFER = 134217728;

#ifndef __MINGW32__
#    define S_ISREG(m) (((m)&_S_IFMT) == _S_IFREG)
#    define S_ISDIR(m) (((m)&_S_IFMT) == _S_IFDIR)
#endif

struct sentry_pathiter_s {
    HANDLE dir_handle;
    const sentry_path_t *parent;
    sentry_path_t *current;
};

static size_t
write_loop(FILE *f, const char *buf, size_t buf_len)
{
    while (buf_len > 0) {
        size_t n = fwrite(buf, 1, buf_len, f);
        if (n == 0 && errno == EINVAL) {
            continue;
        } else if (n < buf_len) {
            break;
        }
        buf += n;
        buf_len -= n;
    }
    fflush(f);
    return buf_len;
}

bool
sentry__filelock_try_lock(sentry_filelock_t *lock)
{
    lock->is_locked = false;

    int fd = _wopen(
        lock->path->path, _O_RDWR | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        return false;
    }

    if (_locking(fd, _LK_NBLCK, 1) != 0) {
        _close(fd);
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
    _locking(lock->fd, LK_UNLCK, 1);
    _close(lock->fd);
    // the remove function will fail if we, or any other process still has an
    // open handle to the file.
    sentry__path_remove(lock->path);
    lock->is_locked = false;
}

static sentry_path_t *
path_with_len(size_t len)
{
    sentry_path_t *rv = SENTRY_MAKE(sentry_path_t);
    rv->path = sentry_malloc(sizeof(wchar_t) * len);
    if (!rv->path) {
        sentry_free(rv);
        return NULL;
    }
    return rv;
}

sentry_path_t *
sentry__path_absolute(const sentry_path_t *path)
{
    wchar_t full[_MAX_PATH];
    if (!_wfullpath(full, path->path, _MAX_PATH)) {
        return NULL;
    }
    return sentry__path_from_wstr(full);
}

sentry_path_t *
sentry__path_current_exe(void)
{
    // inspired by:
    // https://github.com/rust-lang/rust/blob/183e893aaae581bd0ab499ba56b6c5e118557dc7/src/libstd/sys/windows/os.rs#L234-L239
    sentry_path_t *path = path_with_len(MAX_PATH);
    size_t len = GetModuleFileNameW(NULL, path->path, MAX_PATH);
    if (!len) {
        SENTRY_DEBUG("unable to get current exe path");
        sentry__path_free(path);
        return NULL;
    }
    return path;
}

sentry_path_t *
sentry__path_dir(const sentry_path_t *path)
{
    sentry_path_t *dir_path = sentry__path_clone(path);
    if (!dir_path) {
        return NULL;
    }

    // find the filename part and truncate just in front of it if possible
    sentry_pathchar_t *filename
        = (sentry_pathchar_t *)sentry__path_filename(dir_path);
    if (filename > dir_path->path) {
        *(filename - 1) = L'\0';
    }
    return dir_path;
}

sentry_path_t *
sentry__path_from_wstr(const wchar_t *s)
{
    size_t len = wcslen(s) + 1;
    sentry_path_t *rv = path_with_len(len);
    if (rv) {
        memcpy(rv->path, s, len * sizeof(wchar_t));
    }
    return rv;
}

sentry_path_t *
sentry__path_join_wstr(const sentry_path_t *base, const wchar_t *other)
{
    if (isalpha(other[0]) && other[1] == L':') {
        return sentry__path_from_wstr(other);
    } else if (other[0] == L'/' || other[0] == L'\\') {
        if (isalpha(base->path[0]) && base->path[1] == L':') {
            size_t len = wcslen(other) + 3;
            sentry_path_t *rv = path_with_len(len);
            if (!rv) {
                return NULL;
            }
            rv->path[0] = base->path[0];
            rv->path[1] = L':';
            memcpy(rv->path + 2, other, sizeof(wchar_t) * len);
            return rv;
        } else {
            return sentry__path_from_wstr(other);
        }
    } else {
        size_t base_len = wcslen(base->path);
        size_t other_len = wcslen(other);
        size_t len = base_len + other_len + 1;
        bool need_sep = false;
        if (base_len && base->path[base_len - 1] != L'/'
            && base->path[base_len - 1] != L'\\') {
            len += 1;
            need_sep = true;
        }
        sentry_path_t *rv = path_with_len(len);
        if (!rv) {
            return NULL;
        }
        memcpy(rv->path, base->path, sizeof(wchar_t) * base_len);
        if (need_sep) {
            rv->path[base_len] = L'\\';
        }
        memcpy(rv->path + base_len + (need_sep ? 1 : 0), other,
            sizeof(wchar_t) * (other_len + 1));
        return rv;
    }
}

sentry_path_t *
sentry__path_from_str(const char *s)
{
    size_t len = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    sentry_path_t *rv = SENTRY_MAKE(sentry_path_t);
    if (!rv) {
        return NULL;
    }
    rv->path = sentry_malloc(sizeof(wchar_t) * len);
    if (!rv->path) {
        sentry_free(rv);
        return NULL;
    }
    MultiByteToWideChar(CP_ACP, 0, s, -1, rv->path, (int)len);
    return rv;
}

sentry_path_t *
sentry__path_from_str_owned(char *s)
{
    sentry_path_t *rv = sentry__path_from_str(s);
    sentry_free(s);
    return rv;
}

const sentry_pathchar_t *
sentry__path_filename(const sentry_path_t *path)
{
    const wchar_t *s = path->path;
    const wchar_t *ptr = s;
    size_t idx = wcslen(s);

    while (true) {
        if (s[idx] == L'/' || s[idx] == L'\\') {
            ptr = s + idx + 1;
            break;
        }
        if (idx > 0) {
            idx -= 1;
        } else {
            break;
        }
    }

    return ptr;
}

bool
sentry__path_filename_matches(const sentry_path_t *path, const char *filename)
{
    sentry_path_t *fn = sentry__path_from_str(filename);
    bool matches = _wcsicmp(sentry__path_filename(path), fn->path) == 0;
    sentry__path_free(fn);
    return matches;
}

bool
sentry__path_ends_with(const sentry_path_t *path, const char *suffix)
{
    sentry_path_t *s = sentry__path_from_str(suffix);
    size_t pathlen = wcslen(path->path);
    size_t suffixlen = wcslen(s->path);
    if (suffixlen > pathlen) {
        sentry__path_free(s);
        return false;
    }

    bool matches = _wcsicmp(&path->path[pathlen - suffixlen], s->path) == 0;
    sentry__path_free(s);
    return matches;
}

bool
sentry__path_is_dir(const sentry_path_t *path)
{
    struct _stat buf;
    return _wstat(path->path, &buf) == 0 && S_ISDIR(buf.st_mode);
}

bool
sentry__path_is_file(const sentry_path_t *path)
{
    struct _stat buf;
    return _wstat(path->path, &buf) == 0 && S_ISREG(buf.st_mode);
}

size_t
sentry__path_get_size(const sentry_path_t *path)
{
    struct _stat buf;
    if (_wstat(path->path, &buf) == 0 && S_ISREG(buf.st_mode)) {
        return (size_t)buf.st_size;
    } else {
        return 0;
    }
}

sentry_path_t *
sentry__path_append_str(const sentry_path_t *base, const char *suffix)
{
    // convert to wstr
    sentry_path_t *suffix_path = sentry__path_from_str(suffix);
    if (!suffix_path) {
        return NULL;
    }

    // concat into new path
    size_t len_base = wcslen(base->path);
    size_t len_suffix = wcslen(suffix_path->path);
    size_t len = len_base + len_suffix + 1;
    sentry_path_t *rv = path_with_len(len);
    if (rv) {
        memcpy(rv->path, base->path, len_base * sizeof(wchar_t));
        memcpy(rv->path + len_base, suffix_path->path,
            (len_suffix + 1) * sizeof(wchar_t));
    }
    sentry__path_free(suffix_path);

    return rv;
}

sentry_path_t *
sentry__path_join_str(const sentry_path_t *base, const char *other)
{
    sentry_path_t *other_path = sentry__path_from_str(other);
    if (!other_path) {
        return NULL;
    }
    sentry_path_t *rv = sentry__path_join_wstr(base, other_path->path);
    sentry__path_free(other_path);
    return rv;
}

sentry_path_t *
sentry__path_clone(const sentry_path_t *path)
{
    sentry_path_t *rv = SENTRY_MAKE(sentry_path_t);
    if (!rv) {
        return NULL;
    }
    rv->path = _wcsdup(path->path);
    return rv;
}

int
sentry__path_remove(const sentry_path_t *path)
{
    if (!sentry__path_is_dir(path)) {
        if (DeleteFileW(path->path)) {
            return 0;
        }
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : 1;
    } else {
        if (RemoveDirectoryW(path->path)) {
            return 0;
        }
        return 1;
    }
}

int
sentry__path_create_dir_all(const sentry_path_t *path)
{
    wchar_t *p = NULL;
    wchar_t *ptr = NULL;
    int rv = 0;
#define _TRY_MAKE_DIR                                                          \
    do {                                                                       \
        if (!CreateDirectoryW(p, NULL)                                         \
            && GetLastError() != ERROR_ALREADY_EXISTS) {                       \
            rv = 1;                                                            \
            goto done;                                                         \
        }                                                                      \
    } while (0)

    size_t len = wcslen(path->path) + 1;
    p = sentry_malloc(sizeof(wchar_t) * len);
    memcpy(p, path->path, len * sizeof(wchar_t));

    for (ptr = p; *ptr; ptr++) {
        if ((*ptr == L'\\' || *ptr == L'/') && ptr != p && ptr[-1] != L':') {
            *ptr = 0;
            _TRY_MAKE_DIR;
            *ptr = L'\\';
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
    rv->dir_handle = INVALID_HANDLE_VALUE;
    rv->parent = path;
    rv->current = NULL;
    return rv;
}

const sentry_path_t *
sentry__pathiter_next(sentry_pathiter_t *piter)
{
    WIN32_FIND_DATAW data;

    while (true) {
        if (piter->dir_handle == INVALID_HANDLE_VALUE) {
            size_t path_len = wcslen(piter->parent->path);
            wchar_t *pattern = sentry_malloc(sizeof(wchar_t) * (path_len + 3));
            if (!pattern) {
                return NULL;
            }
            memcpy(pattern, piter->parent->path, sizeof(wchar_t) * path_len);
            pattern[path_len] = L'\\';
            pattern[path_len + 1] = L'*';
            pattern[path_len + 2] = 0;
            piter->dir_handle = FindFirstFileW(pattern, &data);
            sentry_free(pattern);
            if (piter->dir_handle == INVALID_HANDLE_VALUE) {
                return NULL;
            }
        } else {
            if (!FindNextFileW(piter->dir_handle, &data)) {
                return NULL;
            }
        }
        if (wcscmp(data.cFileName, L".") == 0
            || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        } else {
            break;
        }
    }

    if (piter->current) {
        sentry__path_free(piter->current);
    }
    piter->current = sentry__path_join_wstr(piter->parent, data.cFileName);
    return piter->current;
}

void
sentry__pathiter_free(sentry_pathiter_t *piter)
{
    if (!piter) {
        return;
    }
    if (piter->dir_handle != INVALID_HANDLE_VALUE) {
        FindClose(piter->dir_handle);
    }
    sentry__path_free(piter->current);
    sentry_free(piter);
}

int
sentry__path_touch(const sentry_path_t *path)
{
    FILE *f = _wfopen(path->path, L"a");
    if (f) {
        fclose(f);
        return 0;
    }
    return 1;
}

char *
sentry__path_read_to_buffer(const sentry_path_t *path, size_t *size_out)
{
    FILE *f = _wfopen(path->path, L"rb");
    if (!f) {
        return NULL;
    }
    size_t len = sentry__path_get_size(path);
    if (len == 0) {
        fclose(f);
        char *rv = sentry_malloc(1);
        rv[0] = '\0';
        if (size_out) {
            *size_out = 0;
        }
        return rv;
    } else if (len > MAX_READ_TO_BUFFER) {
        fclose(f);
        return NULL;
    }

    // this is completely not sane in concurrent situations but hey
    char *rv = sentry_malloc(len + 1);
    if (!rv) {
        fclose(f);
        return NULL;
    }

    size_t remaining = len;
    size_t offset = 0;
    while (remaining > 0) {
        size_t n = fread(rv + offset, 1, remaining, f);
        if (n == 0) {
            break;
        }
        offset += n;
        remaining -= n;
    }

    rv[offset] = '\0';
    fclose(f);

    if (size_out) {
        *size_out = offset;
    }
    return rv;
}

static int
write_buffer_with_mode(const sentry_path_t *path, const char *buf,
    size_t buf_len, const wchar_t *mode)
{
    FILE *f = _wfopen(path->path, mode);
    if (!f) {
        return 1;
    }

    size_t remaining = write_loop(f, buf, buf_len);

    fclose(f);
    return remaining == 0 ? 0 : 1;
}

int
sentry__path_write_buffer(
    const sentry_path_t *path, const char *buf, size_t buf_len)
{
    return write_buffer_with_mode(path, buf, buf_len, L"wb");
}

int
sentry__path_append_buffer(
    const sentry_path_t *path, const char *buf, size_t buf_len)
{
    return write_buffer_with_mode(path, buf, buf_len, L"ab");
}
