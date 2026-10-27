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
#include <wchar.h>

// follow the maximum path length documented here:
// https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
#define MAX_PATH_BUFFER_SIZE 32768

// only read this many bytes to memory ever
// Increased to 512MB to support large minidumps from TSAN/ASAN builds
static const size_t MAX_READ_TO_BUFFER = 536870912;

#ifndef __MINGW32__
#    define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#    define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
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
        }
        if (n < buf_len) {
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

    int fd = _wopen(lock->path->path_w, _O_RDWR | _O_CREAT | _O_TRUNC,
        _S_IREAD | _S_IWRITE);
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

// we assume the `len` parameter to include null-termination
static sentry_path_t *
path_with_len(size_t len)
{
    sentry_path_t *rv = SENTRY_MAKE(sentry_path_t);
    if (!rv) {
        return NULL;
    }
    rv->path = sentry_malloc(sizeof(char) * len);
    if (!rv->path) {
        sentry_free(rv);
        return NULL;
    }
    // let call sites allocate, because the narrow UTF-8 length passed in here
    // comes from strlen which always returns an encoding-dependent byte length,
    // and so we must read the full string to get its wide length.
    rv->path_w = NULL;

    return rv;
}

sentry_path_t *
sentry__path_absolute(const sentry_path_t *path)
{
    wchar_t *path_wstr = path->path_w;
    if (!path_wstr) {
        return NULL;
    }
    // _wfullpath allocates a correctly sized buffer for the absolute path when
    // passing in a NULL buffer. This limits allocation to a minimum (instead of
    // passing in a MAX_PATH_BUFFER_SIZE buffer). But we must clone at the end,
    // so we return a `sentry_malloc()` allocated buffer rather than one from
    // the system allocator (which allows us to open our allocator).
    wchar_t *full = _wfullpath(NULL, path_wstr, 0);
    if (!full) {
        return NULL;
    }
    sentry_path_t *rv = SENTRY_MAKE(sentry_path_t);
    if (!rv) {
        free(full);
        return NULL;
    }
    // we convert the wide-string absolute path back to canonical narrow UTF-8
    rv->path = sentry__string_from_wstr(full);
    if (!rv->path) {
        free(full);
        sentry_free(rv);
        return NULL;
    }
    rv->path_w = sentry__string_clone_wstr(full);
    free(full);
    if (!rv->path_w) {
        sentry_free(rv->path);
        sentry_free(rv);
        return NULL;
    }
    return rv;
}

sentry_path_t *
sentry__path_current_exe(void)
{
    // Let's be safe here and use the "new" 32K character limit for paths.
    wchar_t *path_wstr = sentry_malloc(MAX_PATH_BUFFER_SIZE * sizeof(wchar_t));
    const size_t len
        = GetModuleFileNameW(NULL, path_wstr, MAX_PATH_BUFFER_SIZE);
    if (!len) {
        SENTRY_WARN("unable to get current exe path");
        sentry_free(path_wstr);
        return NULL;
    }
    sentry_path_t *path = SENTRY_MAKE(sentry_path_t);
    if (!path) {
        sentry_free(path_wstr);
        return NULL;
    }
    // convert the path to our canonical narrow UTF-8...
    path->path = sentry__string_from_wstr(path_wstr);
    if (!path->path) {
        sentry_free(path);
        sentry_free(path_wstr);
        return NULL;
    }

    // ...and create a fit-sized buffer for the wide-char so we can free the 64k
    path->path_w = sentry__string_clone_wstr(path_wstr);
    if (!path->path_w) {
        sentry_free(path->path);
        sentry_free(path);
        sentry_free(path_wstr);
        return NULL;
    }
    sentry_free(path_wstr);
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
    char *filename = (char *)sentry__path_filename(dir_path);
    if (filename > dir_path->path) {
        *(filename - 1) = '\0';
    }
    wchar_t *filename_w = (wchar_t *)sentry__path_filename_w(dir_path);
    if (filename_w > dir_path->path_w) {
        *(filename_w - 1) = L'\0';
    }
    return dir_path;
}

sentry_path_t *
sentry__path_from_wstr_n(const wchar_t *s, size_t s_len)
{
    if (!s) {
        return NULL;
    }

    sentry_path_t *rv = SENTRY_MAKE(sentry_path_t);
    if (!rv) {
        return NULL;
    }
    rv->path = sentry__string_from_wstr_n(s, s_len);
    if (!rv->path) {
        sentry_free(rv);
        return NULL;
    }
    rv->path_w = sentry_malloc(sizeof(wchar_t) * (s_len + 1));
    if (!rv->path_w) {
        sentry_free(rv->path);
        sentry_free(rv);
        return NULL;
    }
    if (s_len) {
        wmemcpy(rv->path_w, s, s_len);
    }
    rv->path_w[s_len] = L'\0';

    return rv;
}

sentry_path_t *
sentry__path_from_wstr(const wchar_t *s)
{
    if (!s) {
        return NULL;
    }
    return sentry__path_from_wstr_n(s, wcslen(s));
}

sentry_path_t *
sentry__path_join_wstr(const sentry_path_t *base, const wchar_t *other_w)
{
    char *other = sentry__string_from_wstr(other_w);
    if (!other) {
        return NULL;
    }
    sentry_path_t *rv = sentry__path_join_str(base, other);
    sentry_free(other);
    return rv;
}

const char *
sentry__path_filename(const sentry_path_t *path)
{
    const char *s = path->path;
    const char *ptr = s;
    size_t idx = strlen(s);

    while (true) {
        if (s[idx] == '/' || s[idx] == '\\') {
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

const wchar_t *
sentry__path_filename_w(const sentry_path_t *path)
{
    const wchar_t *s = path->path_w;
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
    if (!fn) {
        return false;
    }
    bool matches = _stricmp(sentry__path_filename(path), fn->path) == 0;
    sentry__path_free(fn);
    return matches;
}

bool
sentry__path_ends_with(const sentry_path_t *path, const char *suffix)
{
    sentry_path_t *s = sentry__path_from_str(suffix);
    if (!s) {
        return false;
    }
    const size_t pathlen = strlen(path->path);
    const size_t suffixlen = strlen(s->path);
    if (suffixlen > pathlen) {
        sentry__path_free(s);
        return false;
    }

    bool matches = _stricmp(&path->path[pathlen - suffixlen], s->path) == 0;
    sentry__path_free(s);
    return matches;
}

bool
sentry__path_is_dir(const sentry_path_t *path)
{
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return false;
    }
    struct _stat buf;
    return _wstat(path_w, &buf) == 0 && S_ISDIR(buf.st_mode);
}

bool
sentry__path_is_file(const sentry_path_t *path)
{
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return false;
    }
    struct _stat buf;
    return _wstat(path_w, &buf) == 0 && S_ISREG(buf.st_mode);
}

bool
sentry__path_is_symlink(const sentry_path_t *path)
{
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path_w);
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

size_t
sentry__path_get_size(const sentry_path_t *path)
{
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return 0;
    }
    struct _stat buf;
    size_t result = 0;
    if (_wstat(path_w, &buf) == 0 && S_ISREG(buf.st_mode)) {
        result = (size_t)buf.st_size;
    }
    return result;
}

time_t
sentry__path_get_mtime(const sentry_path_t *path)
{
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return 0;
    }
    struct _stat buf;
    time_t result = 0;
    if (_wstat(path_w, &buf) == 0) {
        result = (time_t)buf.st_mtime;
    }
    return result;
}

sentry_path_t *
sentry__path_append_str(const sentry_path_t *base, const char *suffix)
{
    sentry_path_t *suffix_path = sentry__path_from_str(suffix);
    if (!suffix_path) {
        return NULL;
    }

    // concat into a new path
    const size_t len_base = strlen(base->path);
    const size_t len_suffix = strlen(suffix_path->path);
    const size_t len = len_base + len_suffix + 1;
    sentry_path_t *rv = path_with_len(len);
    if (rv) {
        memcpy(rv->path, base->path, len_base * sizeof(char));
        memcpy(rv->path + len_base, suffix_path->path,
            (len_suffix + 1) * sizeof(char));

        rv->path_w = sentry__string_to_wstr(rv->path);
        if (!rv->path_w) {
            sentry__path_free(rv);
            return NULL;
        }
    }
    sentry__path_free(suffix_path);

    return rv;
}

sentry_path_t *
sentry__path_join_str(const sentry_path_t *base, const char *other)
{
    // If `other` is an absolute drive path, we just return `other` as a path
    if (isalpha(other[0]) && other[1] == ':') {
        return sentry__path_from_str(other);
    }
    // If `other` is an absolute path without a drive...
    if (other[0] == '/' || other[0] == '\\') {
        // ...we depend on `base` being a drive path to prepend that drive
        if (isalpha(base->path[0]) && base->path[1] == ':') {
            const size_t other_len = strlen(other);
            sentry_path_t *rv = path_with_len(other_len + 3);
            if (!rv) {
                return NULL;
            }
            rv->path[0] = base->path[0];
            rv->path[1] = ':';
            memcpy(rv->path + 2, other, sizeof(char) * other_len);
            rv->path[other_len + 2] = '\0';

            rv->path_w = sentry__string_to_wstr(rv->path);
            if (!rv->path_w) {
                sentry__path_free(rv);
                return NULL;
            }

            return rv;
        }
        // ...or return `other` as an absolute path without a drive specified
        return sentry__path_from_str(other);
    }

    // in all other cases we join the two paths
    const size_t base_len = strlen(base->path);
    const size_t other_len = strlen(other);
    size_t len = base_len + other_len + 1;
    bool need_sep = false;
    if (base_len && base->path[base_len - 1] != '/'
        && base->path[base_len - 1] != '\\') {
        len += 1;
        need_sep = true;
    }
    sentry_path_t *rv = path_with_len(len);
    if (!rv) {
        return NULL;
    }
    memcpy(rv->path, base->path, sizeof(char) * base_len);
    if (need_sep) {
        rv->path[base_len] = '\\';
        rv->path[base_len + 1] = '\0';
    }
    size_t join_offset = base_len + (need_sep ? 1 : 0);
    memcpy(rv->path + join_offset, other, sizeof(char) * (other_len + 1));

    rv->path_w = sentry__string_to_wstr(rv->path);
    if (!rv->path_w) {
        sentry__path_free(rv);
        return NULL;
    }

    return rv;
}

sentry_path_t *
sentry__path_clone(const sentry_path_t *path)
{
    sentry_path_t *rv = SENTRY_MAKE(sentry_path_t);
    if (!rv) {
        return NULL;
    }

    rv->path = sentry__string_clone(path->path);
    rv->path_w = sentry__string_clone_wstr(path->path_w);
    if (!rv->path || !rv->path_w) {
        sentry__path_free(rv);
        return NULL;
    }
    return rv;
}

static int
is_last_error_path_not_found(void)
{
    const DWORD last_error = GetLastError();
    return last_error == ERROR_PATH_NOT_FOUND
        || last_error == ERROR_FILE_NOT_FOUND;
}

int
sentry__path_remove(const sentry_path_t *path)
{
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return 1;
    }
    const BOOL removal_success = sentry__path_is_dir(path)
        ? RemoveDirectoryW(path_w)
        : DeleteFileW(path_w);
    return removal_success ? 0 : !is_last_error_path_not_found();
}

int
sentry__path_rename(const sentry_path_t *src, const sentry_path_t *dst)
{
    wchar_t *src_w = src->path_w;
    wchar_t *dst_w = dst->path_w;
    if (!src_w || !dst_w) {
        return 1;
    }
    // MOVEFILE_REPLACE_EXISTING allows overwriting the destination if it exists
    return MoveFileExW(src_w, dst_w, MOVEFILE_REPLACE_EXISTING) ? 0 : 1;
}

int
sentry__path_copy(const sentry_path_t *src, const sentry_path_t *dst)
{
    if (!src->path_w || !dst->path_w) {
        return 1;
    }
    return CopyFileW(src->path_w, dst->path_w, FALSE) ? 0 : 1;
}

int
sentry__path_create_dir_all(const sentry_path_t *path)
{
    char *p = NULL;
    char *ptr = NULL;
    int rv = 0;
#define _TRY_MAKE_DIR                                                          \
    do {                                                                       \
        wchar_t *p_w = sentry__string_to_wstr(p);                              \
        if (!p_w) {                                                            \
            rv = 1;                                                            \
            goto done;                                                         \
        }                                                                      \
        BOOL success = CreateDirectoryW(p_w, NULL);                            \
        DWORD err = GetLastError();                                            \
        sentry_free(p_w);                                                      \
        if (!success && err != ERROR_ALREADY_EXISTS) {                         \
            rv = 1;                                                            \
            goto done;                                                         \
        }                                                                      \
    } while (0)

    const size_t len = strlen(path->path) + 1;
    p = sentry_malloc(sizeof(char) * len);
    if (!p) {
        return 1;
    }
    memcpy(p, path->path, len * sizeof(char));

    for (ptr = p; *ptr; ptr++) {
        if ((*ptr == '\\' || *ptr == '/') && ptr != p && ptr[-1] != ':') {
            *ptr = 0;
            _TRY_MAKE_DIR;
            *ptr = '\\';
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
            const size_t path_len_w = wcslen(piter->parent->path_w);
            wchar_t *pattern
                = sentry_malloc(sizeof(wchar_t) * (path_len_w + 3));
            if (!pattern) {
                return NULL;
            }
            memcpy(
                pattern, piter->parent->path_w, sizeof(wchar_t) * path_len_w);
            pattern[path_len_w] = L'\\';
            pattern[path_len_w + 1] = L'*';
            pattern[path_len_w + 2] = L'\0';
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
        if (wcscmp(data.cFileName, L".") != 0
            && wcscmp(data.cFileName, L"..") != 0) {
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
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return 1;
    }
    FILE *f = _wfopen(path_w, L"a");
    if (f) {
        fclose(f);
        return 0;
    }
    return 1;
}

char *
sentry__path_read_to_buffer(const sentry_path_t *path, size_t *size_out)
{
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return NULL;
    }
    FILE *f = _wfopen(path_w, L"rb");
    if (!f) {
        return NULL;
    }
    const size_t len = sentry__path_get_size(path);
    if (len == 0) {
        fclose(f);
        char *rv = sentry_malloc(1);
        if (rv) {
            rv[0] = '\0';
        }
        if (size_out) {
            *size_out = 0;
        }
        return rv;
    }
    if (len > MAX_READ_TO_BUFFER) {
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
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return 1;
    }
    FILE *f = _wfopen(path_w, mode);
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

struct sentry_filewriter_s {
    size_t byte_count;
    FILE *f;
    bool failed;
    bool closed;
};

MUST_USE sentry_filewriter_t *
sentry__filewriter_new(const sentry_path_t *path)
{
    wchar_t *path_w = path->path_w;
    if (!path_w) {
        return NULL;
    }
    FILE *f = _wfopen(path_w, L"wb");
    if (!f) {
        return NULL;
    }

    sentry_filewriter_t *result = SENTRY_MAKE(sentry_filewriter_t);
    if (!result) {
        fclose(f);
        return NULL;
    }

    result->f = f;
    result->byte_count = 0;
    result->failed = false;
    result->closed = false;
    return result;
}

size_t
sentry__filewriter_write(
    sentry_filewriter_t *filewriter, const char *buf, size_t buf_len)
{
    if (!filewriter || filewriter->failed || filewriter->closed) {
        return buf_len;
    }
    while (buf_len > 0) {
        size_t n = fwrite(buf, 1, buf_len, filewriter->f);
        if (n == 0 && errno == EINVAL) {
            continue;
        }
        filewriter->byte_count += n;
        buf += n;
        buf_len -= n;
        if (n == 0) {
            filewriter->failed = true;
            break;
        }
    }

    return buf_len;
}

bool
sentry__filewriter_close(sentry_filewriter_t *filewriter)
{
    if (!filewriter) {
        return false;
    }
    if (filewriter->closed) {
        return !filewriter->failed;
    }

    filewriter->closed = true;
    if (fflush(filewriter->f) != 0) {
        filewriter->failed = true;
    }
    if (fclose(filewriter->f) != 0) {
        filewriter->failed = true;
    }
    filewriter->f = NULL;
    return !filewriter->failed;
}

bool
sentry__filewriter_has_failed(const sentry_filewriter_t *filewriter)
{
    return !filewriter || filewriter->failed;
}

void
sentry__filewriter_free(sentry_filewriter_t *filewriter)
{
    if (!filewriter) {
        return;
    }
    sentry__filewriter_close(filewriter);
    sentry_free(filewriter);
}

size_t
sentry__filewriter_byte_count(const sentry_filewriter_t *filewriter)
{
    return filewriter ? filewriter->byte_count : 0;
}
