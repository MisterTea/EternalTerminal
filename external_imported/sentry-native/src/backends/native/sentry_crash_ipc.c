#include "sentry_crash_ipc.h"

#include "sentry_alloc.h"
#include "sentry_logger.h"
#include "sentry_sync.h"

#include <stdio.h>
#include <string.h>

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)

#    include <errno.h>
#    include <fcntl.h>
#    include <pthread.h>
#    include <sys/file.h>
#    include <sys/stat.h>
#    include <unistd.h>

sentry_crash_ipc_t *
sentry__crash_ipc_init_app(sem_t *init_sem)
{
    sentry_crash_ipc_t *ipc = SENTRY_MAKE(sentry_crash_ipc_t);
    if (!ipc) {
        return NULL;
    }
    ipc->is_daemon = false;
    ipc->init_sem = init_sem; // Use provided semaphore (managed by backend)

    // Create shared memory with unique name based on PID and thread ID
    // macOS has a 31-character limit for POSIX shared memory names (PSEMNAMLEN)
    // Format: /s-{8_hex_chars} = 11 chars total (well under 31 limit)
    // We mix PID and TID to create a unique 32-bit identifier, allowing
    // multiple sentry_init() calls from different threads in the same process.
    uint64_t tid = (uint64_t)pthread_self();
    uint32_t id = (uint32_t)((getpid() ^ (tid & 0xFFFFFFFF)) & 0xFFFFFFFF);
    snprintf(ipc->shm_name, sizeof(ipc->shm_name), "/s-%08x", id);

    // Acquire semaphore for exclusive access during initialization
    if (ipc->init_sem && sem_wait(ipc->init_sem) < 0) {
        SENTRY_WARNF(
            "failed to acquire initialization semaphore: %s", strerror(errno));
        sentry_free(ipc);
        return NULL;
    }

    // Try to create or open shared memory
    bool shm_exists = false;
    ipc->shm_fd = shm_open(ipc->shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (ipc->shm_fd < 0 && errno == EEXIST) {
        // Shared memory already exists - reuse it
        shm_exists = true;
        ipc->shm_fd = shm_open(ipc->shm_name, O_RDWR, 0600);
    }

    if (ipc->shm_fd < 0) {
        SENTRY_WARNF("failed to open shared memory: %s", strerror(errno));
        if (ipc->init_sem) {
            sem_post(ipc->init_sem);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Verify and resize shared memory (both new and existing)
    if (shm_exists) {
        // Check if existing shared memory has correct size
        struct stat st;
        if (fstat(ipc->shm_fd, &st) < 0) {
            SENTRY_WARNF("failed to stat shared memory: %s", strerror(errno));
            close(ipc->shm_fd);
            if (ipc->init_sem) {
                sem_post(ipc->init_sem);
            }
            sentry_free(ipc);
            return NULL;
        }
        if (st.st_size != SENTRY_CRASH_SHM_SIZE) {
            // Existing shm has wrong size, resize it
            if (ftruncate(ipc->shm_fd, SENTRY_CRASH_SHM_SIZE) < 0) {
                SENTRY_WARNF("failed to resize existing shared memory: %s",
                    strerror(errno));
                close(ipc->shm_fd);
                if (ipc->init_sem) {
                    sem_post(ipc->init_sem);
                }
                sentry_free(ipc);
                return NULL;
            }
        }
    } else {
        // New shared memory, set size
        if (ftruncate(ipc->shm_fd, SENTRY_CRASH_SHM_SIZE) < 0) {
            SENTRY_WARNF("failed to resize shared memory: %s", strerror(errno));
            close(ipc->shm_fd);
            shm_unlink(ipc->shm_name);
            if (ipc->init_sem) {
                sem_post(ipc->init_sem);
            }
            sentry_free(ipc);
            return NULL;
        }
    }

    // Map shared memory
    ipc->shmem = mmap(NULL, SENTRY_CRASH_SHM_SIZE, PROT_READ | PROT_WRITE,
        MAP_SHARED, ipc->shm_fd, 0);
    if (ipc->shmem == MAP_FAILED) {
        SENTRY_WARNF("failed to map shared memory: %s", strerror(errno));
        close(ipc->shm_fd);
        if (!shm_exists) {
            shm_unlink(ipc->shm_name);
        }
        if (ipc->init_sem) {
            sem_post(ipc->init_sem);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Create eventfd for crash notifications
    ipc->notify_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ipc->notify_fd < 0) {
        SENTRY_WARNF("failed to create eventfd: %s", strerror(errno));
        munmap(ipc->shmem, SENTRY_CRASH_SHM_SIZE);
        close(ipc->shm_fd);
        if (!shm_exists) {
            shm_unlink(ipc->shm_name);
        }
        if (ipc->init_sem) {
            sem_post(ipc->init_sem);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Create eventfd for daemon ready signal
    ipc->ready_fd = eventfd(0, EFD_CLOEXEC);
    if (ipc->ready_fd < 0) {
        SENTRY_WARNF("failed to create ready eventfd: %s", strerror(errno));
        close(ipc->notify_fd);
        munmap(ipc->shmem, SENTRY_CRASH_SHM_SIZE);
        close(ipc->shm_fd);
        if (!shm_exists) {
            shm_unlink(ipc->shm_name);
        }
        if (ipc->init_sem) {
            sem_post(ipc->init_sem);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Initialize shared memory only if newly created
    if (!shm_exists) {
        memset(ipc->shmem, 0, SENTRY_CRASH_SHM_SIZE);
        ipc->shmem->magic = SENTRY_CRASH_MAGIC;
        ipc->shmem->version = SENTRY_CRASH_VERSION;
        sentry__atomic_store(&ipc->shmem->state, SENTRY_CRASH_STATE_READY);
        sentry__atomic_store(&ipc->shmem->sequence, 0);
    }

    // Release semaphore after initialization
    if (ipc->init_sem) {
        sem_post(ipc->init_sem);
    }

    SENTRY_DEBUGF("initialized crash IPC (shm=%s, notify_fd=%d)", ipc->shm_name,
        ipc->notify_fd);

    return ipc;
}

sentry_crash_ipc_t *
sentry__crash_ipc_init_daemon(
    pid_t app_pid, uint64_t app_tid, int notify_eventfd, int ready_eventfd)
{
    sentry_crash_ipc_t *ipc = SENTRY_MAKE(sentry_crash_ipc_t);
    if (!ipc) {
        return NULL;
    }
    ipc->is_daemon = true;

    // Open existing shared memory created by app (using PID and thread ID)
    // Must match the format in sentry__crash_ipc_init_app
    uint32_t id = (uint32_t)((app_pid ^ (app_tid & 0xFFFFFFFF)) & 0xFFFFFFFF);
    snprintf(ipc->shm_name, sizeof(ipc->shm_name), "/s-%08x", id);

    ipc->shm_fd = shm_open(ipc->shm_name, O_RDWR, 0600);
    if (ipc->shm_fd < 0) {
        SENTRY_WARNF(
            "daemon: failed to open shared memory: %s", strerror(errno));
        sentry_free(ipc);
        return NULL;
    }

    // Map shared memory
    ipc->shmem = mmap(NULL, SENTRY_CRASH_SHM_SIZE, PROT_READ | PROT_WRITE,
        MAP_SHARED, ipc->shm_fd, 0);
    if (ipc->shmem == MAP_FAILED) {
        SENTRY_WARNF(
            "daemon: failed to map shared memory: %s", strerror(errno));
        close(ipc->shm_fd);
        sentry_free(ipc);
        return NULL;
    }

    // Validate shared memory
    if (ipc->shmem->magic != SENTRY_CRASH_MAGIC) {
        SENTRY_WARN("daemon: invalid shared memory magic");
        munmap(ipc->shmem, SENTRY_CRASH_SHM_SIZE);
        close(ipc->shm_fd);
        sentry_free(ipc);
        return NULL;
    }

    // Eventfds are inherited from parent after fork - assign them
    ipc->notify_fd = notify_eventfd;
    ipc->ready_fd = ready_eventfd;

    SENTRY_DEBUGF("daemon: attached to crash IPC (shm=%s, notify_fd=%d, "
                  "ready_notify_fd=%d)",
        ipc->shm_name, notify_eventfd, ready_eventfd);

    return ipc;
}

void
sentry__crash_ipc_notify(sentry_crash_ipc_t *ipc)
{
    if (!ipc || ipc->notify_fd < 0) {
        return;
    }

    // Write to eventfd to wake up daemon
    // This is signal-safe
    uint64_t val = 1;
    ssize_t written = write(ipc->notify_fd, &val, sizeof(val));
    (void)written; // Ignore errors in signal handler
}

bool
sentry__crash_ipc_wait(sentry_crash_ipc_t *ipc, int timeout_ms)
{
    if (!ipc || ipc->notify_fd < 0) {
        return false;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ipc->notify_fd, &readfds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(ipc->notify_fd + 1, &readfds, NULL, NULL,
        timeout_ms >= 0 ? &timeout : NULL);

    if (ret > 0 && FD_ISSET(ipc->notify_fd, &readfds)) {
        uint64_t val;
        ssize_t result = read(ipc->notify_fd, &val, sizeof(val));
        if (result < 0) {
            SENTRY_WARN("Failed to read from notify_fd");
        }
        return true;
    }

    return false;
}

void
sentry__crash_ipc_unlink(sentry_crash_ipc_t *ipc)
{
    if (ipc && ipc->shm_name[0]) {
        shm_unlink(ipc->shm_name);
    }
}

void
sentry__crash_ipc_free(sentry_crash_ipc_t *ipc)
{
    if (!ipc) {
        return;
    }

    if (ipc->shmem && ipc->shmem != MAP_FAILED) {
        munmap(ipc->shmem, SENTRY_CRASH_SHM_SIZE);
    }

    if (ipc->shm_fd >= 0) {
        close(ipc->shm_fd);
    }

    if (!ipc->is_daemon) {
        sentry__crash_ipc_unlink(ipc);
    }

    if (ipc->notify_fd >= 0) {
        close(ipc->notify_fd);
    }

    if (ipc->ready_fd >= 0) {
        close(ipc->ready_fd);
    }

    sentry_free(ipc);
}

#elif defined(SENTRY_PLATFORM_MACOS)

#    include <errno.h>
#    include <fcntl.h>
#    include <pthread.h>
#    include <stdlib.h>
#    include <sys/file.h>
#    include <sys/stat.h>
#    include <unistd.h>

sentry_crash_ipc_t *
sentry__crash_ipc_init_app(sentry_mutex_t *init_mutex)
{
    sentry_crash_ipc_t *ipc = SENTRY_MAKE(sentry_crash_ipc_t);
    if (!ipc) {
        return NULL;
    }
    ipc->is_daemon = false;
    ipc->init_mutex = init_mutex;

    // Build a file path for shared memory using the system temp directory.
    // Unlike shm_open(), regular files in $TMPDIR work inside App Sandbox.
    // $TMPDIR is per-app in sandbox (e.g. .../Containers/.../T/).
    uint64_t tid = (uint64_t)pthread_self();
    uint32_t id = (uint32_t)((getpid() ^ (tid & 0xFFFFFFFF)) & 0xFFFFFFFF);
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) {
        tmpdir = "/tmp";
    }
    snprintf(ipc->shm_path, sizeof(ipc->shm_path), "%s/.sentry-shm-%08x",
        tmpdir, id);

    // Acquire mutex for exclusive access during initialization
    if (ipc->init_mutex) {
        sentry__mutex_lock(ipc->init_mutex);
    }

    // Use file-backed shared memory (sandbox-safe, no shm_open)
    bool shm_exists = false;
    ipc->shm_fd = open(ipc->shm_path, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (ipc->shm_fd < 0 && errno == EEXIST) {
        shm_exists = true;
        ipc->shm_fd = open(ipc->shm_path, O_RDWR, 0600);
    }

    if (ipc->shm_fd < 0) {
        SENTRY_WARNF("failed to open shared memory file: %s", strerror(errno));
        if (ipc->init_mutex) {
            sentry__mutex_unlock(ipc->init_mutex);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Verify and resize shared memory (both new and existing)
    if (shm_exists) {
        struct stat st;
        if (fstat(ipc->shm_fd, &st) < 0) {
            SENTRY_WARNF(
                "failed to stat shared memory file: %s", strerror(errno));
            close(ipc->shm_fd);
            if (ipc->init_mutex) {
                sentry__mutex_unlock(ipc->init_mutex);
            }
            sentry_free(ipc);
            return NULL;
        }
        if (st.st_size != SENTRY_CRASH_SHM_SIZE) {
            if (ftruncate(ipc->shm_fd, SENTRY_CRASH_SHM_SIZE) < 0) {
                SENTRY_WARNF(
                    "failed to resize shared memory file: %s", strerror(errno));
                close(ipc->shm_fd);
                if (ipc->init_mutex) {
                    sentry__mutex_unlock(ipc->init_mutex);
                }
                sentry_free(ipc);
                return NULL;
            }
        }
    } else {
        if (ftruncate(ipc->shm_fd, SENTRY_CRASH_SHM_SIZE) < 0) {
            SENTRY_WARNF(
                "failed to resize shared memory file: %s", strerror(errno));
            close(ipc->shm_fd);
            unlink(ipc->shm_path);
            if (ipc->init_mutex) {
                sentry__mutex_unlock(ipc->init_mutex);
            }
            sentry_free(ipc);
            return NULL;
        }
    }

    ipc->shmem = mmap(NULL, SENTRY_CRASH_SHM_SIZE, PROT_READ | PROT_WRITE,
        MAP_SHARED, ipc->shm_fd, 0);
    if (ipc->shmem == MAP_FAILED) {
        SENTRY_WARNF("failed to map shared memory: %s", strerror(errno));
        close(ipc->shm_fd);
        if (!shm_exists) {
            unlink(ipc->shm_path);
        }
        if (ipc->init_mutex) {
            sentry__mutex_unlock(ipc->init_mutex);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Create pipe for crash notifications (works across fork/posix_spawn)
    if (pipe(ipc->notify_pipe) < 0) {
        SENTRY_WARNF("failed to create notification pipe: %s", strerror(errno));
        munmap(ipc->shmem, SENTRY_CRASH_SHM_SIZE);
        close(ipc->shm_fd);
        if (!shm_exists) {
            unlink(ipc->shm_path);
        }
        if (ipc->init_mutex) {
            sentry__mutex_unlock(ipc->init_mutex);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Make write end non-blocking for signal-safe writes
    fcntl(ipc->notify_pipe[1], F_SETFL, O_NONBLOCK);

    // Create pipe for daemon ready signal
    if (pipe(ipc->ready_pipe) < 0) {
        SENTRY_WARNF("failed to create ready pipe: %s", strerror(errno));
        close(ipc->notify_pipe[0]);
        close(ipc->notify_pipe[1]);
        munmap(ipc->shmem, SENTRY_CRASH_SHM_SIZE);
        close(ipc->shm_fd);
        if (!shm_exists) {
            unlink(ipc->shm_path);
        }
        if (ipc->init_mutex) {
            sentry__mutex_unlock(ipc->init_mutex);
        }
        sentry_free(ipc);
        return NULL;
    }

    if (!shm_exists) {
        memset(ipc->shmem, 0, SENTRY_CRASH_SHM_SIZE);
        ipc->shmem->magic = SENTRY_CRASH_MAGIC;
        ipc->shmem->version = SENTRY_CRASH_VERSION;
        sentry__atomic_store(&ipc->shmem->state, SENTRY_CRASH_STATE_READY);
        sentry__atomic_store(&ipc->shmem->sequence, 0);
    }

    if (ipc->init_mutex) {
        sentry__mutex_unlock(ipc->init_mutex);
    }

    SENTRY_DEBUGF("initialized crash IPC (shm=%s, pipe=%d/%d)", ipc->shm_path,
        ipc->notify_pipe[0], ipc->notify_pipe[1]);

    return ipc;
}

sentry_crash_ipc_t *
sentry__crash_ipc_init_daemon(pid_t app_pid, uint64_t app_tid,
    int notify_pipe_read, int ready_pipe_write, int shm_fd)
{
    (void)app_pid;
    (void)app_tid;

    sentry_crash_ipc_t *ipc = SENTRY_MAKE(sentry_crash_ipc_t);
    if (!ipc) {
        return NULL;
    }
    ipc->is_daemon = true;

    // Use the inherited shm_fd directly (no shm_open needed, sandbox-safe)
    ipc->shm_fd = shm_fd;
    ipc->shmem = mmap(NULL, SENTRY_CRASH_SHM_SIZE, PROT_READ | PROT_WRITE,
        MAP_SHARED, ipc->shm_fd, 0);
    if (ipc->shmem == MAP_FAILED) {
        SENTRY_WARNF(
            "daemon: failed to map shared memory: %s", strerror(errno));
        close(ipc->shm_fd);
        sentry_free(ipc);
        return NULL;
    }

    if (ipc->shmem->magic != SENTRY_CRASH_MAGIC) {
        SENTRY_WARN("daemon: invalid shared memory magic");
        munmap(ipc->shmem, SENTRY_CRASH_SHM_SIZE);
        close(ipc->shm_fd);
        sentry_free(ipc);
        return NULL;
    }

    // Pipes and shm_fd are inherited from parent via posix_spawn
    ipc->notify_pipe[0] = notify_pipe_read;
    ipc->notify_pipe[1] = -1;
    ipc->ready_pipe[0] = -1;
    ipc->ready_pipe[1] = ready_pipe_write;

    SENTRY_DEBUGF("daemon: attached to crash IPC (shm_fd=%d, notify_pipe=%d, "
                  "ready_pipe=%d)",
        shm_fd, notify_pipe_read, ready_pipe_write);

    return ipc;
}

void
sentry__crash_ipc_notify(sentry_crash_ipc_t *ipc)
{
    if (!ipc) {
        return;
    }

    // Write byte to pipe (signal-safe)
    char byte = 1;
    write(ipc->notify_pipe[1], &byte, 1);
}

bool
sentry__crash_ipc_wait(sentry_crash_ipc_t *ipc, int timeout_ms)
{
    if (!ipc) {
        return false;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ipc->notify_pipe[0], &readfds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(ipc->notify_pipe[0] + 1, &readfds, NULL, NULL,
        timeout_ms >= 0 ? &timeout : NULL);

    if (result > 0) {
        // Read and discard the byte
        char byte;
        read(ipc->notify_pipe[0], &byte, 1);
        return true;
    }

    return false;
}

void
sentry__crash_ipc_unlink(sentry_crash_ipc_t *ipc)
{
    if (ipc && ipc->shm_path[0]) {
        unlink(ipc->shm_path);
    }
}

void
sentry__crash_ipc_free(sentry_crash_ipc_t *ipc)
{
    if (!ipc) {
        return;
    }

    if (ipc->shmem && ipc->shmem != MAP_FAILED) {
        munmap(ipc->shmem, SENTRY_CRASH_SHM_SIZE);
    }

    if (ipc->shm_fd >= 0) {
        close(ipc->shm_fd);
    }

    // Close pipes
    if (ipc->notify_pipe[0] >= 0) {
        close(ipc->notify_pipe[0]);
    }
    if (ipc->notify_pipe[1] >= 0) {
        close(ipc->notify_pipe[1]);
    }

    // Close ready pipes
    if (ipc->ready_pipe[0] >= 0) {
        close(ipc->ready_pipe[0]);
    }
    if (ipc->ready_pipe[1] >= 0) {
        close(ipc->ready_pipe[1]);
    }

    if (!ipc->is_daemon) {
        sentry__crash_ipc_unlink(ipc);
    }

    sentry_free(ipc);
}

#elif defined(SENTRY_PLATFORM_WINDOWS)

sentry_crash_ipc_t *
sentry__crash_ipc_init_app(HANDLE init_mutex)
{
    sentry_crash_ipc_t *ipc = SENTRY_MAKE(sentry_crash_ipc_t);
    if (!ipc) {
        return NULL;
    }
    ipc->is_daemon = false;
    ipc->init_mutex = init_mutex; // Use provided mutex (managed by backend)

    // Create named shared memory with unique name based on PID and thread ID
    uint64_t tid = (uint64_t)GetCurrentThreadId();
    swprintf(ipc->shm_name, SENTRY_CRASH_IPC_NAME_SIZE,
        L"Local\\SentryCrash-%lu-%llx", GetCurrentProcessId(), tid);

    // Log the shared memory name
    char *shm_name_utf8 = sentry__string_from_wstr(ipc->shm_name);
    if (shm_name_utf8) {
        SENTRY_DEBUGF("APP: Creating shared memory: %s", shm_name_utf8);
        sentry_free(shm_name_utf8);
    }

    // Acquire mutex for exclusive access during initialization
    if (ipc->init_mutex) {
        DWORD result = WaitForSingleObject(ipc->init_mutex, INFINITE);
        if (result != WAIT_OBJECT_0) {
            SENTRY_WARNF(
                "failed to acquire initialization mutex: %lu", GetLastError());
            sentry_free(ipc);
            return NULL;
        }
    }

    // Try to create or open shared memory
    bool shm_exists = false;
    ipc->shm_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
        PAGE_READWRITE, 0, SENTRY_CRASH_SHM_SIZE, ipc->shm_name);
    if (!ipc->shm_handle) {
        SENTRY_WARNF("failed to create shared memory: %lu", GetLastError());
        if (ipc->init_mutex) {
            ReleaseMutex(ipc->init_mutex);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Check if shared memory already existed
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        shm_exists = true;
    }

    ipc->shmem = MapViewOfFile(
        ipc->shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, SENTRY_CRASH_SHM_SIZE);
    if (!ipc->shmem) {
        SENTRY_WARNF("failed to map shared memory: %lu", GetLastError());
        CloseHandle(ipc->shm_handle);
        if (ipc->init_mutex) {
            ReleaseMutex(ipc->init_mutex);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Create named event for notifications (using PID and thread ID)
    swprintf(ipc->event_name, SENTRY_CRASH_IPC_NAME_SIZE,
        L"Local\\SentryCrashEvent-%lu-%llx", GetCurrentProcessId(), tid);

    // Log the event name
    char *event_name_utf8 = sentry__string_from_wstr(ipc->event_name);
    if (event_name_utf8) {
        SENTRY_DEBUGF("APP: Creating event: %s", event_name_utf8);
        sentry_free(event_name_utf8);
    }

    ipc->event_handle = CreateEventW(NULL, FALSE, FALSE, ipc->event_name);
    if (!ipc->event_handle) {
        SENTRY_WARNF("failed to create event: %lu", GetLastError());
        UnmapViewOfFile(ipc->shmem);
        CloseHandle(ipc->shm_handle);
        if (ipc->init_mutex) {
            ReleaseMutex(ipc->init_mutex);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Create ready event for daemon to signal when it's initialized (using PID
    // and thread ID)
    swprintf(ipc->ready_event_name, SENTRY_CRASH_IPC_NAME_SIZE,
        L"Local\\SentryCrashReady-%lu-%llx", GetCurrentProcessId(), tid);
    ipc->ready_event_handle = CreateEventW(
        NULL, TRUE, FALSE, ipc->ready_event_name); // Manual-reset
    if (!ipc->ready_event_handle) {
        SENTRY_WARNF("failed to create ready event: %lu", GetLastError());
        CloseHandle(ipc->event_handle);
        UnmapViewOfFile(ipc->shmem);
        CloseHandle(ipc->shm_handle);
        if (ipc->init_mutex) {
            ReleaseMutex(ipc->init_mutex);
        }
        sentry_free(ipc);
        return NULL;
    }

    // Initialize shared memory only if newly created
    if (!shm_exists) {
        memset(ipc->shmem, 0, SENTRY_CRASH_SHM_SIZE);
        ipc->shmem->magic = SENTRY_CRASH_MAGIC;
        ipc->shmem->version = SENTRY_CRASH_VERSION;
        sentry__atomic_store(&ipc->shmem->state, SENTRY_CRASH_STATE_READY);
        sentry__atomic_store(&ipc->shmem->sequence, 0);
    }

    // Release mutex after initialization
    if (ipc->init_mutex) {
        ReleaseMutex(ipc->init_mutex);
    }

    SENTRY_DEBUG("initialized crash IPC");

    return ipc;
}

sentry_crash_ipc_t *
sentry__crash_ipc_init_daemon(pid_t app_pid, uint64_t app_tid,
    HANDLE event_handle, HANDLE ready_event_handle)
{
    // On Windows, we open events by name, so handles from parent are not used
    // (handles are per-process and cannot be directly inherited)
    (void)event_handle;
    (void)ready_event_handle;

    sentry_crash_ipc_t *ipc = SENTRY_MAKE(sentry_crash_ipc_t);
    if (!ipc) {
        return NULL;
    }
    ipc->is_daemon = true;

    // Open existing shared memory (using PID and thread ID)
    swprintf(ipc->shm_name, SENTRY_CRASH_IPC_NAME_SIZE,
        L"Local\\SentryCrash-%lu-%llx", (unsigned long)app_pid, app_tid);

    ipc->shm_handle
        = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, ipc->shm_name);
    if (!ipc->shm_handle) {
        SENTRY_WARNF(
            "daemon: failed to open shared memory: %lu", GetLastError());
        sentry_free(ipc);
        return NULL;
    }

    ipc->shmem = MapViewOfFile(
        ipc->shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, SENTRY_CRASH_SHM_SIZE);
    if (!ipc->shmem) {
        SENTRY_WARNF(
            "daemon: failed to map shared memory: %lu", GetLastError());
        CloseHandle(ipc->shm_handle);
        sentry_free(ipc);
        return NULL;
    }

    if (ipc->shmem->magic != SENTRY_CRASH_MAGIC) {
        SENTRY_WARN("daemon: invalid shared memory magic");
        UnmapViewOfFile(ipc->shmem);
        CloseHandle(ipc->shm_handle);
        sentry_free(ipc);
        return NULL;
    }

    // Open existing event (using PID and thread ID)
    swprintf(ipc->event_name, SENTRY_CRASH_IPC_NAME_SIZE,
        L"Local\\SentryCrashEvent-%lu-%llx", (unsigned long)app_pid, app_tid);

    ipc->event_handle = OpenEventW(SYNCHRONIZE, FALSE, ipc->event_name);
    if (!ipc->event_handle) {
        SENTRY_WARNF("daemon: failed to open event: %lu", GetLastError());
        UnmapViewOfFile(ipc->shmem);
        CloseHandle(ipc->shm_handle);
        sentry_free(ipc);
        return NULL;
    }

    // Open ready event to signal when daemon is initialized (using PID and
    // thread ID)
    swprintf(ipc->ready_event_name, SENTRY_CRASH_IPC_NAME_SIZE,
        L"Local\\SentryCrashReady-%lu-%llx", (unsigned long)app_pid, app_tid);
    ipc->ready_event_handle
        = OpenEventW(EVENT_MODIFY_STATE, FALSE, ipc->ready_event_name);
    if (!ipc->ready_event_handle) {
        SENTRY_WARNF("daemon: failed to open ready event: %lu", GetLastError());
        CloseHandle(ipc->event_handle);
        UnmapViewOfFile(ipc->shmem);
        CloseHandle(ipc->shm_handle);
        sentry_free(ipc);
        return NULL;
    }

    SENTRY_DEBUG("daemon: attached to crash IPC");

    return ipc;
}

void
sentry__crash_ipc_notify(sentry_crash_ipc_t *ipc)
{
    if (!ipc || !ipc->event_handle) {
        // No logging - called from signal handler/exception filter
        return;
    }

    // SetEvent is safe to call from exception filter
    // Ignore errors silently - we're crashing anyway
    SetEvent(ipc->event_handle);
}

bool
sentry__crash_ipc_wait(sentry_crash_ipc_t *ipc, int timeout_ms)
{
    if (!ipc || !ipc->event_handle) {
        SENTRY_WARN("crash_ipc_wait: ipc or event_handle is NULL");
        return false;
    }

    DWORD timeout = (timeout_ms >= 0) ? (DWORD)timeout_ms : INFINITE;
    DWORD result = WaitForSingleObject(ipc->event_handle, timeout);

    if (result == WAIT_OBJECT_0) {
        return true;
    } else if (result == WAIT_TIMEOUT) {
        return false;
    } else {
        SENTRY_WARNF("crash_ipc_wait: unexpected result %lu, error %lu", result,
            GetLastError());
        return false;
    }
}

void
sentry__crash_ipc_unlink(sentry_crash_ipc_t *ipc)
{
    (void)ipc;
}

void
sentry__crash_ipc_free(sentry_crash_ipc_t *ipc)
{
    if (!ipc) {
        return;
    }

    if (ipc->shmem) {
        UnmapViewOfFile(ipc->shmem);
    }

    if (ipc->shm_handle) {
        CloseHandle(ipc->shm_handle);
    }

    if (ipc->event_handle) {
        CloseHandle(ipc->event_handle);
    }

    if (ipc->ready_event_handle) {
        CloseHandle(ipc->ready_event_handle);
    }

    if (ipc->parent_handle) {
        CloseHandle(ipc->parent_handle);
    }

    sentry_free(ipc);
}

#endif

// Cross-platform ready signaling functions
void
sentry__crash_ipc_signal_ready(sentry_crash_ipc_t *ipc)
{
    if (!ipc) {
        SENTRY_WARN("signal_ready: ipc is NULL");
        return;
    }

#if defined(SENTRY_PLATFORM_WINDOWS)
    if (!ipc->ready_event_handle) {
        SENTRY_WARN("signal_ready: ready_event_handle is NULL");
        return;
    }
    if (!SetEvent(ipc->ready_event_handle)) {
        SENTRY_WARNF("daemon: SetEvent failed: %lu", GetLastError());
    } else {
        SENTRY_DEBUG("daemon: Successfully signaled ready to parent");
    }
#elif defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    // Signal via eventfd
    uint64_t val = 1;
    if (write(ipc->ready_fd, &val, sizeof(val)) < 0) {
        SENTRY_WARNF(
            "daemon: write to ready_eventfd failed: %s", strerror(errno));
    } else {
        SENTRY_DEBUG("daemon: signaled ready to parent");
    }
#elif defined(SENTRY_PLATFORM_MACOS)
    // Signal via pipe
    char byte = 1;
    if (write(ipc->ready_pipe[1], &byte, 1) < 0) {
        SENTRY_WARNF("daemon: write to ready_pipe failed: %s", strerror(errno));
    } else {
        SENTRY_DEBUG("daemon: signaled ready to parent");
    }
#endif
}

bool
sentry__crash_ipc_wait_for_ready(sentry_crash_ipc_t *ipc, int timeout_ms)
{
    if (!ipc) {
        return false;
    }

#if defined(SENTRY_PLATFORM_WINDOWS)
    if (!ipc->ready_event_handle) {
        SENTRY_WARN("No ready event handle");
        return false;
    }

    DWORD timeout = (timeout_ms >= 0) ? (DWORD)timeout_ms : INFINITE;
    DWORD result = WaitForSingleObject(ipc->ready_event_handle, timeout);

    if (result == WAIT_OBJECT_0) {
        return true;
    } else if (result == WAIT_TIMEOUT) {
        return false;
    } else {
        SENTRY_WARNF(
            "crash_ipc_wait_for_ready: unexpected result %lu, error %lu",
            result, GetLastError());
        return false;
    }
#elif defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    // Wait on ready_eventfd with poll/select
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ipc->ready_fd, &readfds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(ipc->ready_fd + 1, &readfds, NULL, NULL,
        timeout_ms >= 0 ? &timeout : NULL);

    if (result > 0) {
        // Read the eventfd value
        uint64_t val;
        if (read(ipc->ready_fd, &val, sizeof(val)) < 0) {
            SENTRY_WARNF("read from ready_eventfd failed: %s", strerror(errno));
            return false;
        }
        return true;
    } else if (result == 0) {
        return false; // Timeout
    } else {
        SENTRY_WARNF("select on ready_eventfd failed: %s", strerror(errno));
        return false;
    }
#elif defined(SENTRY_PLATFORM_MACOS)
    // Wait on ready_pipe with select
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ipc->ready_pipe[0], &readfds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(ipc->ready_pipe[0] + 1, &readfds, NULL, NULL,
        timeout_ms >= 0 ? &timeout : NULL);

    if (result > 0) {
        // Read and discard the byte
        char byte;
        if (read(ipc->ready_pipe[0], &byte, 1) < 0) {
            SENTRY_WARNF("read from ready_pipe failed: %s", strerror(errno));
            return false;
        }
        return true;
    } else if (result == 0) {
        return false; // Timeout
    } else {
        SENTRY_WARNF("select on ready_pipe failed: %s", strerror(errno));
        return false;
    }
#else
    return false;
#endif
}
