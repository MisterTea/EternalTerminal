#include "sentry_boot.h"

#if defined(SENTRY_PLATFORM_UNIX)
#    include <errno.h>
#    include <fcntl.h>
#    include <pthread.h>
#    include <signal.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#    if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
#        include <sys/prctl.h>
#    endif
#elif defined(SENTRY_PLATFORM_WINDOWS) && !defined(SENTRY_PLATFORM_XBOX)
#    include <werapi.h>
#endif

#include <string.h>

#include "sentry_alloc.h"
#include "sentry_backend.h"
#include "sentry_core.h"
#include "sentry_crash_context.h"
#include "sentry_crash_daemon.h"
#include "sentry_crash_handler.h"
#include "sentry_crash_ipc.h"
#include "sentry_database.h"
#include "sentry_envelope.h"
#include "sentry_json.h"
#include "sentry_logger.h"
#include "sentry_logs.h"
#include "sentry_metrics.h"
#include "sentry_options.h"
#include "sentry_os.h"
#include "sentry_path.h"

#include "sentry_scope.h"
#include "sentry_session.h"
#include "sentry_sync.h"
#include "sentry_tracing.h"
#include "sentry_transport.h"
#include "sentry_value.h"
#include "transports/sentry_disk_transport.h"

// Global process-wide synchronization for IPC and shared memory access
// This lives for the entire backend lifetime and is shared across all threads
#if defined(SENTRY_PLATFORM_WINDOWS)
static HANDLE g_ipc_mutex = NULL;
#elif defined(SENTRY_PLATFORM_MACOS)
// macOS uses a plain pthread mutex instead of named semaphores (sem_open)
// because App Sandbox blocks POSIX named semaphores.
static sentry_mutex_t g_ipc_sync_mutex = SENTRY__MUTEX_INIT;
#else
#    include <semaphore.h>
static sem_t *g_ipc_init_sem = SEM_FAILED;
static char g_ipc_sem_name[64] = { 0 };
#endif

// Mutex to protect IPC initialization (Windows and Linux only, not macOS/iOS)
// macOS uses g_ipc_sync_mutex directly; iOS has no out-of-process daemon.
#if defined(SENTRY_PLATFORM_WINDOWS)                                           \
    || (!defined(SENTRY_PLATFORM_MACOS) && !defined(SENTRY_PLATFORM_IOS))
#    ifdef SENTRY__MUTEX_INIT_DYN
SENTRY__MUTEX_INIT_DYN(g_ipc_init_mutex)
#    else
static sentry_mutex_t g_ipc_init_mutex = SENTRY__MUTEX_INIT;
#    endif
#endif

#if defined(SENTRY_PLATFORM_WINDOWS) && !defined(SENTRY_PLATFORM_XBOX)
static sentry_wer_registration_t g_wer_registration = { 0 };

static sentry_path_t *g_wer_path = NULL;

static LSTATUS
wer_set_registry_value(const sentry_path_t *wer_path, DWORD value)
{
    return RegSetKeyValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Windows Error Reporting\\"
        L"RuntimeExceptionHelperModules",
        wer_path->path_w, REG_DWORD, &value, sizeof(value));
}

static LSTATUS
wer_delete_registry_value(const sentry_path_t *wer_path)
{
    return RegDeleteKeyValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Windows Error Reporting\\"
        L"RuntimeExceptionHelperModules",
        wer_path->path_w);
}

static sentry_path_t *
wer_default_path(void)
{
    sentry_path_t *current_exe = sentry__path_current_exe();
    if (!current_exe) {
        return NULL;
    }

    sentry_path_t *exe_dir = sentry__path_dir(current_exe);
    sentry__path_free(current_exe);
    if (!exe_dir) {
        return NULL;
    }

    sentry_path_t *wer_path = sentry__path_join_str(exe_dir, "sentry-wer.dll");
    sentry__path_free(exe_dir);
    return wer_path;
}

static void
wer_unregister_module(void)
{
    if (!g_wer_path) {
        return;
    }

    WerUnregisterRuntimeExceptionModule(
        g_wer_path->path_w, &g_wer_registration);
    wer_delete_registry_value(g_wer_path);
    sentry__path_free(g_wer_path);
    g_wer_path = NULL;
    memset(&g_wer_registration, 0, sizeof(g_wer_registration));
}

static void
wer_register_module(uint64_t app_tid)
{
    windows_version_t win_ver;
    if (!sentry__get_windows_version(&win_ver) || win_ver.build < 19041) {
        SENTRY_WARN("Native WER module not registered, because Windows "
                    "doesn't meet version requirements (build >= 19041).");
        return;
    }

    sentry_path_t *wer_path = wer_default_path();
    if (!wer_path || !sentry__path_is_file(wer_path)) {
        SENTRY_WARN("Native WER module not found");
        sentry__path_free(wer_path);
        return;
    }

    const DWORD one = 1;
    LSTATUS reg_res = wer_set_registry_value(wer_path, one);
    if (reg_res != ERROR_SUCCESS) {
        SENTRY_WARN("registering native WER module in registry failed");
        sentry__path_free(wer_path);
        return;
    }

    g_wer_registration.version = 1;
    g_wer_registration.app_pid = GetCurrentProcessId();
    g_wer_registration.app_tid = app_tid;

    HRESULT hr = WerRegisterRuntimeExceptionModule(
        wer_path->path_w, &g_wer_registration);
    if (FAILED(hr)) {
        SENTRY_WARN("registering native WER module failed");
        wer_delete_registry_value(wer_path);
        sentry__path_free(wer_path);
        memset(&g_wer_registration, 0, sizeof(g_wer_registration));
        return;
    }

    SENTRY_DEBUGF("registered native WER module \"%s\"", wer_path->path);
    g_wer_path = wer_path;
}

#endif

/**
 * Native backend state
 */
typedef struct {
    sentry_crash_ipc_t *ipc;
    pid_t daemon_pid;
    sentry_path_t *event_path;
    sentry_path_t *breadcrumb1_path;
    sentry_path_t *breadcrumb2_path;
    sentry_path_t *envelope_path;
    size_t num_breadcrumbs;
    volatile long crashed;
} native_backend_state_t;

static int
native_backend_startup(
    sentry_backend_t *backend, const sentry_options_t *options)
{
    SENTRY_WARN("The native backend is experimental and under active "
                "development.");
    SENTRY_DEBUG("starting native backend");

#if defined(SENTRY_PLATFORM_WINDOWS)
    // Create process-wide mutex for IPC synchronization (Windows)
    // Use portable mutex to protect Windows mutex creation
    SENTRY__MUTEX_INIT_DYN_ONCE(g_ipc_init_mutex);
    sentry__mutex_lock(&g_ipc_init_mutex);

    if (!g_ipc_mutex) {
        wchar_t mutex_name[64];
        swprintf(
            mutex_name, 64, L"Local\\SentryIPC-%lu", GetCurrentProcessId());
        g_ipc_mutex = CreateMutexW(NULL, FALSE, mutex_name);
        if (!g_ipc_mutex) {
            sentry__mutex_unlock(&g_ipc_init_mutex);
            SENTRY_WARNF("failed to create IPC mutex: %lu", GetLastError());
            return 1;
        }
    }

    sentry__mutex_unlock(&g_ipc_init_mutex);
#elif defined(SENTRY_PLATFORM_MACOS)
    // macOS uses a plain pthread mutex (no sem_open which is blocked by App
    // Sandbox). The mutex is statically initialized - no setup needed.
    (void)0;
#elif !defined(SENTRY_PLATFORM_IOS)
    // Create process-wide IPC initialization semaphore (singleton pattern)
    // Protected by mutex to handle concurrent backend startups
    SENTRY__MUTEX_INIT_DYN_ONCE(g_ipc_init_mutex);
    sentry__mutex_lock(&g_ipc_init_mutex);

    if (g_ipc_init_sem == SEM_FAILED) {
        snprintf(g_ipc_sem_name, sizeof(g_ipc_sem_name), "/sentry-init-%d",
            (int)getpid());
        // Unlink any stale semaphore from previous runs
        sem_unlink(g_ipc_sem_name);
        // Create fresh semaphore with initial value 1
        g_ipc_init_sem = sem_open(g_ipc_sem_name, O_CREAT | O_EXCL, 0600, 1);
        if (g_ipc_init_sem == SEM_FAILED) {
            sentry__mutex_unlock(&g_ipc_init_mutex);
            SENTRY_WARNF("failed to create IPC semaphore: %s", strerror(errno));
            return 1;
        }
    }

    sentry__mutex_unlock(&g_ipc_init_mutex);
#endif

    native_backend_state_t *state = SENTRY_MAKE(native_backend_state_t);
    if (!state) {
        return 1;
    }
    backend->data = state;

    // Initialize IPC (protected by global synchronization for concurrent
    // access)
#if defined(SENTRY_PLATFORM_WINDOWS)
    state->ipc = sentry__crash_ipc_init_app(g_ipc_mutex);
#elif defined(SENTRY_PLATFORM_IOS)
    state->ipc = sentry__crash_ipc_init_app(NULL);
#elif defined(SENTRY_PLATFORM_MACOS)
    state->ipc = sentry__crash_ipc_init_app(&g_ipc_sync_mutex);
#else
    state->ipc = sentry__crash_ipc_init_app(g_ipc_init_sem);
#endif
    if (!state->ipc) {
        SENTRY_WARN("failed to initialize crash IPC");
        sentry_free(state);
        backend->data = NULL;
        return 1;
    }

    // Configure crash context (protected by synchronization for concurrent
    // access)
#if defined(SENTRY_PLATFORM_WINDOWS)
    if (g_ipc_mutex) {
        DWORD wait_result = WaitForSingleObject(g_ipc_mutex, INFINITE);
        if (wait_result != WAIT_OBJECT_0) {
            SENTRY_WARNF("failed to acquire mutex for context setup: %lu",
                GetLastError());
            sentry__crash_ipc_free(state->ipc);
            sentry_free(state);
            backend->data = NULL;
            return 1;
        }
    }
#elif defined(SENTRY_PLATFORM_MACOS)
    sentry__mutex_lock(&g_ipc_sync_mutex);
#elif !defined(SENTRY_PLATFORM_IOS)
    if (g_ipc_init_sem && sem_wait(g_ipc_init_sem) < 0) {
        SENTRY_WARNF("failed to acquire semaphore for context setup: %s",
            strerror(errno));
        sentry__crash_ipc_free(state->ipc);
        sentry_free(state);
        backend->data = NULL;
        return 1;
    }
#endif

    sentry_crash_context_t *ctx = state->ipc->shmem;

    // Set minidump mode from options
    ctx->minidump_mode = (sentry_minidump_mode_t)options->minidump_mode;

    // Set crash reporting mode from options
    ctx->crash_reporting_mode = options->crash_reporting_mode;
    ctx->system_crash_reporter_enabled = options->system_crash_reporter_enabled;
    ctx->crash_upload_mode = options->crash_upload_mode;

    // Pass debug logging setting to daemon
    ctx->debug_enabled = options->debug;
    ctx->attach_screenshot = options->attach_screenshot;
    ctx->attach_session_replay = options->attach_session_replay;
    ctx->session_replay_duration = options->session_replay_duration;
    ctx->cache_keep = (int)options->cache_keep;
    ctx->require_user_consent = options->require_user_consent;
    ctx->enable_large_attachments = options->enable_large_attachments;
    ctx->http_retry = options->http_retry;
    ctx->shutdown_timeout = options->shutdown_timeout;
    ctx->transfer_timeout = options->transfer_timeout;
    ctx->max_breadcrumbs = (uint32_t)options->max_breadcrumbs;
    sentry__atomic_store(
        &ctx->user_consent, sentry__atomic_fetch(&options->run->user_consent));

    // Set up event and breadcrumb paths
    sentry_path_t *run_path = options->run->run_path;
    sentry_path_t *db_path = options->database_path;

    // Store database path for daemon use
    if (db_path) {
#ifdef _WIN32
        strncpy_s(ctx->database_path, sizeof(ctx->database_path), db_path->path,
            _TRUNCATE);
#else
        strncpy(
            ctx->database_path, db_path->path, sizeof(ctx->database_path) - 1);
        ctx->database_path[sizeof(ctx->database_path) - 1] = '\0';
#endif
    }

    // Store DSN for daemon to send crashes
    if (options->dsn && options->dsn->raw) {
#ifdef _WIN32
        strncpy_s(ctx->dsn, sizeof(ctx->dsn), options->dsn->raw, _TRUNCATE);
#else
        strncpy(ctx->dsn, options->dsn->raw, sizeof(ctx->dsn) - 1);
        ctx->dsn[sizeof(ctx->dsn) - 1] = '\0';
#endif
    }

    // Store transport configuration for daemon's curl transport
    if (options->ca_certs) {
#ifdef _WIN32
        strncpy_s(
            ctx->ca_certs, sizeof(ctx->ca_certs), options->ca_certs, _TRUNCATE);
#else
        strncpy(ctx->ca_certs, options->ca_certs, sizeof(ctx->ca_certs) - 1);
        ctx->ca_certs[sizeof(ctx->ca_certs) - 1] = '\0';
#endif
    }

    if (options->proxy) {
#ifdef _WIN32
        strncpy_s(ctx->proxy, sizeof(ctx->proxy), options->proxy, _TRUNCATE);
#else
        strncpy(ctx->proxy, options->proxy, sizeof(ctx->proxy) - 1);
        ctx->proxy[sizeof(ctx->proxy) - 1] = '\0';
#endif
    }

    if (options->user_agent) {
#ifdef _WIN32
        strncpy_s(ctx->user_agent, sizeof(ctx->user_agent), options->user_agent,
            _TRUNCATE);
#else
        strncpy(
            ctx->user_agent, options->user_agent, sizeof(ctx->user_agent) - 1);
        ctx->user_agent[sizeof(ctx->user_agent) - 1] = '\0';
#endif
    }

    state->event_path = sentry__path_join_str(run_path, "__sentry-event");
    state->breadcrumb1_path
        = sentry__path_join_str(run_path, "__sentry-breadcrumb1");
    state->breadcrumb2_path
        = sentry__path_join_str(run_path, "__sentry-breadcrumb2");

    sentry__path_touch(state->event_path);
    sentry__path_touch(state->breadcrumb1_path);
    sentry__path_touch(state->breadcrumb2_path);

    // Copy paths to crash context
#ifdef _WIN32
    strncpy_s(ctx->event_path, sizeof(ctx->event_path), state->event_path->path,
        _TRUNCATE);
    strncpy_s(ctx->breadcrumb1_path, sizeof(ctx->breadcrumb1_path),
        state->breadcrumb1_path->path, _TRUNCATE);
    strncpy_s(ctx->breadcrumb2_path, sizeof(ctx->breadcrumb2_path),
        state->breadcrumb2_path->path, _TRUNCATE);
#else
    strncpy(
        ctx->event_path, state->event_path->path, sizeof(ctx->event_path) - 1);
    ctx->event_path[sizeof(ctx->event_path) - 1] = '\0';
    strncpy(ctx->breadcrumb1_path, state->breadcrumb1_path->path,
        sizeof(ctx->breadcrumb1_path) - 1);
    ctx->breadcrumb1_path[sizeof(ctx->breadcrumb1_path) - 1] = '\0';
    strncpy(ctx->breadcrumb2_path, state->breadcrumb2_path->path,
        sizeof(ctx->breadcrumb2_path) - 1);
    ctx->breadcrumb2_path[sizeof(ctx->breadcrumb2_path) - 1] = '\0';
#endif

    // Set up crash envelope path
    state->envelope_path = sentry__path_join_str(
        options->run->run_path, "__sentry-crash.envelope");
    if (state->envelope_path) {
#ifdef _WIN32
        strncpy_s(ctx->envelope_path, sizeof(ctx->envelope_path),
            state->envelope_path->path, _TRUNCATE);
#else
        strncpy(ctx->envelope_path, state->envelope_path->path,
            sizeof(ctx->envelope_path) - 1);
        ctx->envelope_path[sizeof(ctx->envelope_path) - 1] = '\0';
#endif
    }

    // Set up external crash reporter if configured
    if (options->external_crash_reporter) {
#ifdef _WIN32
        strncpy_s(ctx->external_reporter_path,
            sizeof(ctx->external_reporter_path),
            options->external_crash_reporter->path, _TRUNCATE);
#else
        strncpy(ctx->external_reporter_path,
            options->external_crash_reporter->path,
            sizeof(ctx->external_reporter_path) - 1);
        ctx->external_reporter_path[sizeof(ctx->external_reporter_path) - 1]
            = '\0';
#endif
    }

#if defined(SENTRY_PLATFORM_WINDOWS)
    // Release mutex after context configuration
    if (g_ipc_mutex) {
        ReleaseMutex(g_ipc_mutex);
    }
#elif defined(SENTRY_PLATFORM_MACOS)
    sentry__mutex_unlock(&g_ipc_sync_mutex);
#elif !defined(SENTRY_PLATFORM_IOS)
    // Release semaphore after context configuration
    if (g_ipc_init_sem) {
        sem_post(g_ipc_init_sem);
    }
#endif

    // Install crash handlers (signal handlers on Linux/macOS, Mach exception
    // handler on iOS)
#if defined(SENTRY_PLATFORM_IOS)
    if (sentry__crash_handler_init(state->ipc) < 0) {
        SENTRY_WARN("failed to initialize crash handler");
        sentry__crash_ipc_free(state->ipc);
        sentry_free(state);
        backend->data = NULL;
        return 1;
    }
#else
    // Other platforms: Use out-of-process daemon
    // Pass the notification handles (eventfd/pipe on Unix, events on Windows)
    const char *daemon_handler_path
        = options->handler_path ? options->handler_path->path : NULL;
#    if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    uint64_t tid = (uint64_t)pthread_self();
    state->daemon_pid = sentry__crash_daemon_start(getpid(), tid,
        state->ipc->notify_fd, state->ipc->ready_fd, daemon_handler_path);
#    elif defined(SENTRY_PLATFORM_MACOS)
    uint64_t tid = (uint64_t)pthread_self();
    state->daemon_pid
        = sentry__crash_daemon_start(getpid(), tid, state->ipc->notify_pipe[0],
            state->ipc->ready_pipe[1], state->ipc->shm_fd, daemon_handler_path);
#    elif defined(SENTRY_PLATFORM_WINDOWS)
    uint64_t tid = (uint64_t)GetCurrentThreadId();
    state->daemon_pid = sentry__crash_daemon_start(GetCurrentProcessId(), tid,
        state->ipc->event_handle, state->ipc->ready_event_handle,
        daemon_handler_path);
#    endif

    // On Windows, pid_t is DWORD (unsigned), so (pid_t)-1 == 0xFFFFFFFF.
    // On Unix, pid_t is signed and fork returns -1 on failure.
#    if defined(SENTRY_PLATFORM_WINDOWS)
    if (state->daemon_pid == (pid_t)-1) {
#    else
    if (state->daemon_pid < 0) {
#    endif
        SENTRY_WARN("failed to start crash daemon");
        sentry__crash_ipc_free(state->ipc);
        sentry_free(state);
        backend->data = NULL;
        return 1;
    }

    SENTRY_DEBUGF("crash daemon started with PID %d", state->daemon_pid);

#    if defined(SENTRY_PLATFORM_MACOS)
    // Close unused pipe ends in parent process
    close(state->ipc->notify_pipe[0]); // Daemon reads from this
    close(state->ipc->ready_pipe[1]); // Daemon writes to this
    state->ipc->notify_pipe[0] = -1;
    state->ipc->ready_pipe[1] = -1;
#    endif

#    if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    // Close unused eventfd ends in parent process
    // (eventfds are bidirectional, but we only use one direction per fd)
    // Parent writes to notify_fd, daemon reads from it - parent can close for
    // reading Daemon writes to ready_fd, parent reads from it - parent can
    // close for writing Actually, eventfds can't be closed for one direction,
    // so keep them open

    // On Linux, allow the daemon to ptrace this process
    // This is required when Yama LSM ptrace_scope is enabled
    if (prctl(PR_SET_PTRACER, state->daemon_pid, 0, 0, 0) != 0) {
        SENTRY_WARNF(
            "prctl(PR_SET_PTRACER) failed: %s - daemon may not be able to "
            "read process memory",
            strerror(errno));
    } else {
        SENTRY_DEBUGF("Set daemon PID %d as ptracer", state->daemon_pid);
    }
#    endif

    // Wait for daemon to signal it's ready
    if (!sentry__crash_ipc_wait_for_ready(
            state->ipc, SENTRY_CRASH_DAEMON_READY_TIMEOUT_MS)) {
        SENTRY_WARN("Daemon did not signal ready in time, proceeding anyway");
    } else {
        SENTRY_DEBUG("Daemon signaled ready");
    }

#    if defined(SENTRY_PLATFORM_WINDOWS) && !defined(SENTRY_PLATFORM_XBOX)
    wer_register_module(tid);
#    endif

    if (sentry__crash_handler_init(state->ipc) < 0) {
        SENTRY_WARN("failed to initialize crash handler");
#    if defined(SENTRY_PLATFORM_UNIX)
        kill(state->daemon_pid, SIGTERM);
#    elif defined(SENTRY_PLATFORM_WINDOWS)
#        if !defined(SENTRY_PLATFORM_XBOX)
        wer_unregister_module();
#        endif
        // On Windows, terminate the daemon process
        HANDLE hDaemon
            = OpenProcess(PROCESS_TERMINATE, FALSE, state->daemon_pid);
        if (hDaemon) {
            TerminateProcess(hDaemon, 1);
            CloseHandle(hDaemon);
        }
#    endif
        sentry__crash_ipc_free(state->ipc);
        sentry_free(state);
        backend->data = NULL;
        return 1;
    }
#endif

    SENTRY_DEBUG("native backend started successfully");
    return 0;
}

static void
native_backend_shutdown(sentry_backend_t *backend)
{
    SENTRY_DEBUG("shutting down native backend");

    native_backend_state_t *state = (native_backend_state_t *)backend->data;
    if (!state) {
        return;
    }

#if defined(SENTRY_PLATFORM_WINDOWS) && !defined(SENTRY_PLATFORM_XBOX)
    wer_unregister_module();
#endif

    // Shutdown crash handlers (signal handlers on Linux/macOS, Mach exception
    // handler on iOS)
    sentry__crash_handler_shutdown();

#if defined(SENTRY_PLATFORM_UNIX) && !defined(SENTRY_PLATFORM_IOS)
    // Terminate daemon (Unix)
    if (state->daemon_pid > 0) {
        kill(state->daemon_pid, SIGTERM);
        // Wait for daemon to exit
        waitpid(state->daemon_pid, NULL, 0);
    }
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // Terminate daemon (Windows)
    if (state->daemon_pid > 0) {
        HANDLE hDaemon = OpenProcess(
            PROCESS_TERMINATE | SYNCHRONIZE, FALSE, state->daemon_pid);
        if (hDaemon) {
            TerminateProcess(hDaemon, 0);
            // Wait for daemon to exit (with timeout)
            WaitForSingleObject(hDaemon, 5000); // 5 second timeout
            CloseHandle(hDaemon);
        }
    }
#endif

    // Dump daemon log file for debugging (especially useful in CI)
    // Use same naming as shared memory to find the correct log file
    if (state->ipc && state->ipc->shmem) {
        char log_path[SENTRY_CRASH_MAX_PATH];
        int log_path_len = -1;

        // Extract the unique ID from the shm name/path to find the daemon log
        // Platform-specific: shm_name on Linux/Windows, shm_path on macOS
#if defined(SENTRY_PLATFORM_WINDOWS)
        const wchar_t *shm_id_w = wcsrchr(state->ipc->shm_name, L'-');
        if (shm_id_w) {
            shm_id_w++; // Skip the '-'
            char *shm_id = sentry__string_from_wstr(shm_id_w);
            if (shm_id) {
                log_path_len = _snprintf(log_path, sizeof(log_path),
                    "%s\\sentry-daemon-%s.log",
                    state->ipc->shmem->database_path, shm_id);
                if (log_path_len > 0 && log_path_len < (int)sizeof(log_path)) {
                    wchar_t *wpath = sentry__string_to_wstr(log_path);
                    FILE *log_file = wpath ? _wfopen(wpath, L"r") : NULL;
                    sentry_free(wpath);
                    if (log_file) {
                        fprintf(stderr,
                            "\n========== Daemon Log (%s) ==========\n",
                            shm_id);
                        char line[1024];
                        while (fgets(line, sizeof(line), log_file)) {
                            fprintf(stderr, "%s", line);
                        }
                        fprintf(stderr,
                            "=========================================\n\n");
                        fclose(log_file);
                    }
                }
                sentry_free(shm_id);
            }
        }
#else
        // On macOS: shm_path = "{tmpdir}/.sentry-shm-{id}"
        // On Linux: shm_name = "/s-{id}"
        // In both cases, the ID follows the last '-'
#    if defined(SENTRY_PLATFORM_MACOS)
        const char *shm_id_src = state->ipc->shm_path;
#    else
        const char *shm_id_src = state->ipc->shm_name;
#    endif
        const char *shm_id = shm_id_src[0] ? strrchr(shm_id_src, '-') : NULL;
        if (shm_id) {
            shm_id++; // Skip the '-'
            log_path_len = snprintf(log_path, sizeof(log_path),
                "%s/sentry-daemon-%s.log", state->ipc->shmem->database_path,
                shm_id);
            if (log_path_len > 0 && log_path_len < (int)sizeof(log_path)) {
                FILE *log_file = fopen(log_path, "r");
                if (log_file) {
                    fprintf(stderr, "\n========== Daemon Log (%s) ==========\n",
                        shm_id);
                    char line[1024];
                    while (fgets(line, sizeof(line), log_file)) {
                        fprintf(stderr, "%s", line);
                    }
                    fprintf(stderr,
                        "=========================================\n\n");
                    fclose(log_file);
                }
            }
        }
#endif
    }

    // Cleanup IPC
    if (state->ipc) {
        sentry__crash_ipc_free(state->ipc);
        state->ipc = NULL; // Prevent use-after-free
    }

#if !defined(SENTRY_PLATFORM_WINDOWS) && !defined(SENTRY_PLATFORM_IOS)
    // Don't clean up semaphore here - it persists for the process lifetime
    // and may be reused if backend is restarted within same process
#endif

    SENTRY_DEBUG("native backend shutdown complete");
}

static void
native_backend_user_consent_changed(sentry_backend_t *backend)
{
    native_backend_state_t *state = (native_backend_state_t *)backend->data;
    if (!state || !state->ipc || !state->ipc->shmem) {
        return;
    }
    SENTRY_WITH_OPTIONS (options) {
        if (options->run) {
            sentry__atomic_store(&state->ipc->shmem->user_consent,
                sentry__atomic_fetch(&options->run->user_consent));
        }
    }
}

static void
native_backend_free(sentry_backend_t *backend)
{
    native_backend_state_t *state = (native_backend_state_t *)backend->data;
    if (!state) {
        return;
    }

    sentry__path_free(state->event_path);
    sentry__path_free(state->breadcrumb1_path);
    sentry__path_free(state->breadcrumb2_path);
    sentry__path_free(state->envelope_path);

    sentry_free(state);
}

// Writes the scope's attachment list to <run>/__sentry-attachments so the
// crash daemon can locate and append them to the crash envelope.
static void
native_backend_write_attachments(const sentry_path_t *event_path)
{
    if (!event_path) {
        return;
    }
    SENTRY_WITH_SCOPE (scope) {
        if (!scope->attachments) {
            continue;
        }
        sentry_path_t *run_path = sentry__path_dir(event_path);
        if (!run_path) {
            continue;
        }
        sentry_path_t *attach_list_path
            = sentry__path_join_str(run_path, "__sentry-attachments");
        if (attach_list_path) {
            sentry_value_t attach_list = sentry_value_new_list();
            for (sentry_attachment_t *it = scope->attachments; it;
                it = it->next) {
                if (!it->path) {
                    continue;
                }
                sentry_value_t attach_info = sentry_value_new_object();
                sentry_value_set_by_key(attach_info, "path",
                    sentry_value_new_string(it->path->path));
                const char *filename = sentry__path_filename(
                    it->filename ? it->filename : it->path);
                sentry_value_set_by_key(
                    attach_info, "filename", sentry_value_new_string(filename));
                if (it->type && *it->type) {
                    sentry_value_set_by_key(attach_info, "attachment_type",
                        sentry_value_new_string(it->type));
                }
                if (it->content_type) {
                    sentry_value_set_by_key(attach_info, "content_type",
                        sentry_value_new_string(it->content_type));
                }
                sentry_value_append(attach_list, attach_info);
            }
            size_t attach_json_len = 0;
            char *attach_json
                = sentry__value_to_json(attach_list, &attach_json_len);
            sentry_value_decref(attach_list);
            if (attach_json) {
                sentry__path_write_buffer(
                    attach_list_path, attach_json, attach_json_len);
                sentry_free(attach_json);
            }
            sentry__path_free(attach_list_path);
        }
        sentry__path_free(run_path);
    }
}

#if defined(SENTRY_PLATFORM_WINDOWS)
// Sentry's symbolicator needs `contexts.device.arch` to process PE modules. If
// the scope already carries a device context with arch (host SDKs like Unity
// provide one), leave it; otherwise synthesize a minimal one so native-only
// consumers still symbolicate.
static void
ensure_device_arch(sentry_value_t event)
{
    sentry_value_t contexts = sentry_value_get_by_key(event, "contexts");
    if (sentry_value_is_null(contexts)) {
        contexts = sentry_value_new_object();
        sentry_value_set_by_key(event, "contexts", contexts);
    }
    sentry_value_t device = sentry_value_get_by_key(contexts, "device");
    if (sentry_value_is_null(device)) {
        device = sentry_value_new_object();
        sentry_value_set_by_key(
            device, "type", sentry_value_new_string("device"));
        sentry_value_set_by_key(contexts, "device", device);
    }
    if (!sentry_value_is_null(sentry_value_get_by_key(device, "arch"))) {
        return;
    }
#    if defined(_M_AMD64)
    sentry_value_set_by_key(device, "arch", sentry_value_new_string("x86_64"));
#    elif defined(_M_IX86)
    sentry_value_set_by_key(device, "arch", sentry_value_new_string("x86"));
#    elif defined(_M_ARM64)
    sentry_value_set_by_key(device, "arch", sentry_value_new_string("arm64"));
#    endif
}
#endif

static void
native_backend_flush_scope(
    sentry_backend_t *backend, const sentry_options_t *options)
{
    native_backend_state_t *state = (native_backend_state_t *)backend->data;
    if (!state || !state->event_path) {
        return;
    }

    // Manifest writes must continue post-crash so attachments registered
    // from on_crash/before_send reach the daemon
    native_backend_write_attachments(state->event_path);

    if (sentry__atomic_fetch(&state->crashed)) {
        return;
    }

    // Create event with current scope
    sentry_value_t event = sentry_value_new_object();
    sentry_value_set_by_key(
        event, "level", sentry__value_new_level(SENTRY_LEVEL_FATAL));

    // Apply scope with contexts
    SENTRY_WITH_SCOPE (scope) {
        sentry__scope_apply_to_event(scope, options, event, SENTRY_SCOPE_NONE);
    }
#if defined(SENTRY_PLATFORM_WINDOWS)
    ensure_device_arch(event);
#endif

    size_t json_len = 0;
    char *json_str = sentry__value_to_json(event, &json_len);
    sentry_value_decref(event);

    if (json_str) {
        sentry__path_write_buffer(state->event_path, json_str, json_len);
        sentry_free(json_str);
    }
}

static void
native_backend_add_breadcrumb(sentry_backend_t *backend,
    sentry_value_t breadcrumb, const sentry_options_t *options)
{
    native_backend_state_t *state = (native_backend_state_t *)backend->data;
    if (!state) {
        return;
    }

    size_t max_breadcrumbs = options->max_breadcrumbs;
    if (!max_breadcrumbs) {
        return;
    }

    bool first_breadcrumb = state->num_breadcrumbs % max_breadcrumbs == 0;

    const sentry_path_t *breadcrumb_file
        = state->num_breadcrumbs % (max_breadcrumbs * 2) < max_breadcrumbs
        ? state->breadcrumb1_path
        : state->breadcrumb2_path;

    state->num_breadcrumbs++;

    if (!breadcrumb_file) {
        return;
    }

    // Append as msgpack, matching the crashpad backend. msgpack values are
    // self-delimiting, so the daemon can read the concatenated ring file back
    // into a list via `sentry__value_from_msgpack`.
    size_t mpack_size = 0;
    char *mpack = sentry_value_to_msgpack(breadcrumb, &mpack_size);
    if (!mpack) {
        return;
    }

    int rv = first_breadcrumb
        ? sentry__path_write_buffer(breadcrumb_file, mpack, mpack_size)
        : sentry__path_append_buffer(breadcrumb_file, mpack, mpack_size);

    sentry_free(mpack);

    if (rv != 0) {
        SENTRY_WARN("failed to write breadcrumb");
    }
}

/**
 * Ensures that buffer attachments have a unique path in the run directory.
 * Similar to Crashpad's ensure_unique_path function.
 */
static bool
ensure_attachment_path(sentry_attachment_t *attachment)
{
    if (!attachment || !attachment->filename) {
        return false;
    }

    // Generate UUID for unique path
    sentry_uuid_t uuid = sentry_uuid_new_v4();
    char uuid_str[37];
    sentry_uuid_as_string(&uuid, uuid_str);

    sentry_path_t *base_path = NULL;
    SENTRY_WITH_OPTIONS (options) {
        if (options->run && options->run->run_path) {
            base_path = sentry__path_join_str(options->run->run_path, uuid_str);
        }
    }

    if (!base_path || sentry__path_create_dir_all(base_path) != 0) {
        sentry__path_free(base_path);
        return false;
    }

    sentry_path_t *old_path = attachment->path;
    attachment->path = sentry__path_join_str(
        base_path, sentry__path_filename(attachment->filename));

    sentry__path_free(base_path);
    sentry__path_free(old_path);
    return attachment->path != NULL;
}

static void
native_backend_add_attachment(
    sentry_backend_t *backend, sentry_attachment_t *attachment)
{
    (void)backend; // Unused

    // For buffer attachments, assign a path in the run directory and write to
    // disk
    if (attachment->buf) {
        if (!attachment->path) {
            if (!ensure_attachment_path(attachment)) {
                SENTRY_WARN("failed to assign path for buffer attachment");
                return;
            }
        }

        // Write buffer to disk
        if (sentry__path_write_buffer(
                attachment->path, attachment->buf, attachment->buf_len)
            != 0) {
            SENTRY_WARNF("failed to write native backend attachment \"%s\"",
                attachment->path->path);
        }
    }
    // For file attachments, the path is already set and points to the actual
    // file. The crash daemon will read these files from their original
    // locations.
}

/**
 * Handle exception - called from signal handler via sentry_handle_exception
 * This processes the event with on_crash/before_send hooks and ends the session
 */
static void
native_backend_except(sentry_backend_t *backend, const sentry_ucontext_t *uctx)
{
    native_backend_state_t *state = (native_backend_state_t *)backend->data;
    if (state) {
        sentry__atomic_store(&state->crashed, 1);
    }

    SENTRY_WITH_OPTIONS (options) {
        // Disable logging during crash handling if configured
        if (!options->enable_logging_when_crashed) {
            sentry__logger_disable();
        }

        SENTRY_DEBUG("handling native backend exception");

        // Flush logs and metrics in a crash-safe manner before crash handling
        if (options->enable_logs) {
            sentry__logs_flush_crash_safe();
        }
        if (options->enable_metrics) {
            sentry__metrics_flush_crash_safe();
        }

        // Write crash marker
        sentry__write_crash_marker(options);

        sentry_value_t transaction
            = sentry__trace_finish(SENTRY_SPAN_STATUS_ABORTED);

        // Create crash event
        sentry_value_t event = sentry_value_new_event();
        sentry_value_set_by_key(
            event, "level", sentry__value_new_level(SENTRY_LEVEL_FATAL));

        bool should_handle = true;

        // Call on_crash hook if configured
        if (options->on_crash_func) {
            SENTRY_DEBUG("invoking `on_crash` hook");
            sentry_value_t result
                = options->on_crash_func(uctx, event, options->on_crash_data);
            should_handle = !sentry_value_is_null(result);
            event = result;
        }

        if (should_handle) {
            // Apply before_send hook if on_crash wasn't set
            if (!options->on_crash_func && options->before_send_func) {
                SENTRY_DEBUG("invoking `before_send` hook");
                event = options->before_send_func(
                    event, NULL, options->before_send_data);
                should_handle = !sentry_value_is_null(event);
            }

            if (should_handle) {
                // Apply scope to the event. The daemon assembles breadcrumbs
                // from the ring files
                SENTRY_WITH_SCOPE (scope) {
                    sentry__scope_apply_to_event(
                        scope, options, event, SENTRY_SCOPE_NONE);
                }
#if defined(SENTRY_PLATFORM_WINDOWS)
                ensure_device_arch(event);
#endif

#ifndef SENTRY_SCREENSHOT_NONE
                // The screenshot is captured by the daemon out-of-process, so
                // we invoke the hook here (in the crashing process, where
                // user callbacks can run) and communicate the decision to the
                // daemon by flipping attach_screenshot in the shared crash
                // context. Screenshots are only captured on Windows.
                if (options->attach_screenshot
                    && options->before_screenshot_func && state && state->ipc
                    && state->ipc->shmem) {
                    SENTRY_DEBUG("invoking `before_screenshot` hook");
                    if (options->before_screenshot_func(
                            event, options->before_screenshot_data)
                        == 0) {
                        SENTRY_DEBUG("screenshot skipped by "
                                     "`before_screenshot` hook");
                        state->ipc->shmem->attach_screenshot = false;
                    }
                }
#endif

                // Write event as JSON file
                // Daemon will read this and create envelope with minidump
                if (state && state->event_path) {
                    size_t event_json_len = 0;
                    char *event_json
                        = sentry__value_to_json(event, &event_json_len);
                    if (event_json) {
                        int rv = sentry__path_write_buffer(
                            state->event_path, event_json, event_json_len);
                        sentry_free(event_json);
                        if (rv == 0) {
                            SENTRY_DEBUG("Wrote crash event JSON for daemon");
                        } else {
                            SENTRY_WARN("Failed to write event JSON");
                        }
                    }
                }

                sentry_value_decref(event);

                // End session with crashed status and write session envelope to
                // disk
                sentry__record_errors_on_current_session(1);
                sentry_session_t *session
                    = sentry__end_current_session_with_status(
                        SENTRY_SESSION_STATUS_CRASHED);

                if (session || !sentry_value_is_null(transaction)) {
                    sentry_transport_t *disk_transport
                        = sentry_new_disk_transport(options->run);
                    if (disk_transport) {
                        if (!sentry_value_is_null(transaction)) {
                            sentry_envelope_t *tx_envelope
                                = sentry__prepare_transaction(
                                    options, transaction, NULL);
                            if (tx_envelope) {
                                sentry__capture_envelope(
                                    disk_transport, tx_envelope, options);
                            }
                        }
                        if (session) {
                            sentry_envelope_t *envelope
                                = sentry__envelope_new();
                            if (envelope) {
                                sentry__envelope_add_session(envelope, session);
                                sentry__capture_envelope(
                                    disk_transport, envelope, options);
                            }
                        }
                        sentry__transport_dump_queue(
                            disk_transport, options->run);
                        sentry_transport_free(disk_transport);
                    } else {
                        sentry_value_decref(transaction);
                    }
                }

                // Dump any pending transport queue
                sentry__transport_dump_queue(options->transport, options->run);

                SENTRY_DEBUG("crash event and session written, daemon will "
                             "create and send minidump");
            } else {
                sentry_value_decref(transaction);
            }
        } else {
            SENTRY_DEBUG("event was discarded by the `on_crash` hook");
            sentry_value_decref(event);
            sentry_value_decref(transaction);
        }
    }
}

void
sentry__backend_preload(void)
{
}

/**
 * Create native backend
 */
sentry_backend_t *
sentry__backend_new(void)
{
    sentry_backend_t *backend = SENTRY_MAKE(sentry_backend_t);
    if (!backend) {
        return NULL;
    }

    backend->startup_func = native_backend_startup;
    backend->shutdown_func = native_backend_shutdown;
    backend->free_func = native_backend_free;
    backend->except_func = native_backend_except;
    backend->flush_scope_func = native_backend_flush_scope;
    backend->add_breadcrumb_func = native_backend_add_breadcrumb;
    backend->add_attachment_func = native_backend_add_attachment;
    backend->user_consent_changed_func = native_backend_user_consent_changed;
    backend->can_capture_after_shutdown = false;

    return backend;
}
