#ifndef SENTRY_CRASH_DAEMON_H_INCLUDED
#define SENTRY_CRASH_DAEMON_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_crash_ipc.h"

#if defined(SENTRY_PLATFORM_UNIX)
#    include <sys/types.h>
#elif defined(SENTRY_PLATFORM_WINDOWS)
#    include <windows.h>
#endif

// Forward declaration
struct sentry_options_s;

/**
 * Start crash daemon for monitoring app process
 * This forks a child process (Unix) or creates a new process (Windows) that
 * waits for crashes
 *
 * @param app_pid Parent application process ID
 * @param app_tid Parent application thread ID
 * @param notify_handle Crash notification handle
 * @param ready_handle Ready signal handle
 * @return Daemon PID on success, -1 on failure
 */
#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
pid_t sentry__crash_daemon_start(pid_t app_pid, uint64_t app_tid,
    int notify_eventfd, int ready_eventfd, const char *handler_path);
#elif defined(SENTRY_PLATFORM_MACOS)
pid_t sentry__crash_daemon_start(pid_t app_pid, uint64_t app_tid,
    int notify_pipe_read, int ready_pipe_write, int shm_fd,
    const char *handler_path);
#elif defined(SENTRY_PLATFORM_WINDOWS)
pid_t sentry__crash_daemon_start(pid_t app_pid, uint64_t app_tid,
    HANDLE event_handle, HANDLE ready_event_handle, const char *handler_path);
#endif

/**
 * Daemon main loop (runs in forked child on Unix, or separate process on
 * Windows)
 * @param app_pid Parent process ID
 * @param app_tid Parent thread ID
 * @param notify_handle Notification handle for crash signals
 * @param ready_handle Ready signal handle to signal parent
 */
#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
int sentry__crash_daemon_main(
    pid_t app_pid, uint64_t app_tid, int notify_eventfd, int ready_eventfd);
#elif defined(SENTRY_PLATFORM_MACOS)
int sentry__crash_daemon_main(pid_t app_pid, uint64_t app_tid,
    int notify_pipe_read, int ready_pipe_write, int shm_fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
int sentry__crash_daemon_main(pid_t app_pid, uint64_t app_tid,
    HANDLE event_handle, HANDLE ready_event_handle);
#endif

/**
 * Process crash and generate minidump with envelope
 *
 * Called by the crash daemon (out-of-process on Linux/macOS).
 *
 * It writes the minidump, creates an envelope with all attachments,
 * and sends it via transport. Signal-safe, avoids SDK mutexes.
 *
 * @param options Sentry options (DSN, transport, etc.)
 * @param ipc Crash IPC with crash context in shared memory
 * @return true if the crash envelope was captured for upload or reporting
 */
bool sentry__process_crash(
    const struct sentry_options_s *options, sentry_crash_ipc_t *ipc);

#endif
