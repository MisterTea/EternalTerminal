#include "sentry_path.h"
#include "sentry_alloc.h"

void
sentry__path_free(sentry_path_t *path)
{
    if (!path) {
        return;
    }
    sentry_free(path->path);
    sentry_free(path);
}

int
sentry__path_remove_all(const sentry_path_t *path)
{
    if (sentry__path_is_dir(path)) {
        sentry_pathiter_t *piter = sentry__path_iter_directory(path);
        const sentry_path_t *p;
        while ((p = sentry__pathiter_next(piter)) != NULL) {
            sentry__path_remove_all(p);
        }
        sentry__pathiter_free(piter);
    }
    return sentry__path_remove(path);
}

sentry_filelock_t *
sentry__filelock_new(sentry_path_t *path)
{
    sentry_filelock_t *rv = SENTRY_MAKE(sentry_filelock_t);
    if (!rv) {
        sentry__path_free(path);
        return NULL;
    }
    rv->path = path;
    rv->is_locked = false;

    return rv;
}

void
sentry__filelock_free(sentry_filelock_t *lock)
{
    sentry__filelock_unlock(lock);
    sentry__path_free(lock->path);
    sentry_free(lock);
}
