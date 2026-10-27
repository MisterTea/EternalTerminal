#ifndef SENTRY_CRASH_CONTEXT_H_INCLUDED
#define SENTRY_CRASH_CONTEXT_H_INCLUDED

// _XOPEN_SOURCE must be defined before any system header inclusion for
// ucontext_t to be fully exposed on strict POSIX-conforming systems.
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#    define _XOPEN_SOURCE 700
#endif

#include "sentry.h" // For sentry_minidump_mode_t
#include "sentry_boot.h"

#include <limits.h>
#include <stdint.h>

#if defined(SENTRY_PLATFORM_UNIX)
#    include <signal.h>
#    include <sys/types.h>
#    include <ucontext.h>
#    include <unistd.h>
#elif defined(SENTRY_PLATFORM_WINDOWS)
#    include <windows.h>
// MinGW provides pid_t in sys/types.h, MSVC doesn't
#    if defined(__MINGW32__) || defined(__MINGW64__)
#        include <sys/types.h>
#    else
// MSVC doesn't have pid_t - define it as DWORD
typedef DWORD pid_t;
#    endif
#endif

#define SENTRY_CRASH_MAGIC 0x53454E54 // "SENT"
#define SENTRY_CRASH_VERSION 1

// Limits for crash context (used in shared memory and minidump writers)
#define SENTRY_CRASH_MAX_THREADS 256
#define SENTRY_CRASH_MAX_MODULES 2048
#define SENTRY_CRASH_MAX_MAPPINGS 4096

// Max path length in crash context
// Use system PATH_MAX where available (typically 4096 on Linux/macOS, 260 on
// Windows) Fall back to 4096 for safety on systems without PATH_MAX
#if defined(PATH_MAX)
#    define SENTRY_CRASH_MAX_PATH PATH_MAX
#elif defined(MAX_PATH)
#    define SENTRY_CRASH_MAX_PATH MAX_PATH
#else
#    define SENTRY_CRASH_MAX_PATH 4096
#endif

// Buffer sizes for IPC and file operations
#define SENTRY_CRASH_IPC_NAME_SIZE                                             \
    64 // Size for IPC object names (shm, semaphore, event)
#define SENTRY_CRASH_SIGNAL_STACK_SIZE 65536 // 64KB stack for signal handler
#define SENTRY_CRASH_FILE_BUFFER_SIZE (8 * 1024) // 8KB for file I/O operations

// Envelope and header buffer sizes
#define SENTRY_CRASH_ENVELOPE_HEADER_SIZE 1024 // Envelope headers
#define SENTRY_CRASH_ITEM_HEADER_SIZE 256 // Item headers (event, minidump)
#define SENTRY_CRASH_READ_BUFFER_SIZE 8192 // General read buffer

// String formatting buffer sizes
#define SENTRY_CRASH_TIMESTAMP_SIZE 32 // Timestamp strings

// Memory and stack size limits
#define SENTRY_CRASH_MAX_STACK_CAPTURE                                         \
    (512 * 1024) // 512KB default stack capture
#define SENTRY_CRASH_MAX_STACK_SIZE (1024 * 1024) // 1MB max stack size
#define SENTRY_CRASH_MAX_REGION_SIZE                                           \
    (64 * 1024 * 1024) // 64MB max memory region

// Detect sanitizer builds (ASAN/TSAN) which are much slower
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#    define SENTRY_SANITIZER_BUILD 1
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#        define SENTRY_SANITIZER_BUILD 1
#    endif
#endif

// Timeout values for IPC and crash handling (in milliseconds)
// Increased timeout for sanitizer builds which are much slower
#if defined(SENTRY_SANITIZER_BUILD)
#    define SENTRY_CRASH_DAEMON_READY_TIMEOUT_MS 30000 // 30s for TSAN/ASAN
#    define SENTRY_CRASH_HANDLER_WAIT_TIMEOUT_MS 30000 // 30s for TSAN/ASAN
#else
#    define SENTRY_CRASH_DAEMON_READY_TIMEOUT_MS 10000 // 10s for daemon startup
#    define SENTRY_CRASH_HANDLER_WAIT_TIMEOUT_MS                               \
        10000 // 10s max wait for daemon
#endif
#define SENTRY_CRASH_DAEMON_WAIT_TIMEOUT_MS                                    \
    5000 // 5 seconds between daemon health checks
#define SENTRY_CRASH_HANDLER_POLL_INTERVAL_MS                                  \
    100 // 100ms poll interval in exception handler

// Compatibility macros for arm64e (PAC - Pointer Authentication Codes).
// On arm64e, __darwin_arm_thread_state64 uses opaque members for pointer
// registers (__fp, __lr, __sp, __pc) and requires accessor macros.
// These wrappers use Apple's macros when available, falling back to direct
// member access on plain arm64.
// Note: only GET macros are provided. For copying full register state between
// mcontext structs, use struct assignment which preserves raw PAC-signed
// values.
#if defined(SENTRY_PLATFORM_MACOS) && defined(__aarch64__)
#    if defined(__DARWIN_OPAQUE_ARM_THREAD_STATE64)
#        define SENTRY__ARM64_GET_FP(ss) __darwin_arm_thread_state64_get_fp(ss)
#        define SENTRY__ARM64_GET_LR(ss) __darwin_arm_thread_state64_get_lr(ss)
#        define SENTRY__ARM64_GET_SP(ss) __darwin_arm_thread_state64_get_sp(ss)
#        define SENTRY__ARM64_GET_PC(ss) __darwin_arm_thread_state64_get_pc(ss)
#    else
#        define SENTRY__ARM64_GET_FP(ss) ((ss).__fp)
#        define SENTRY__ARM64_GET_LR(ss) ((ss).__lr)
#        define SENTRY__ARM64_GET_SP(ss) ((ss).__sp)
#        define SENTRY__ARM64_GET_PC(ss) ((ss).__pc)
#    endif
#endif

/**
 * Crash state machine for atomic coordination between app and daemon
 */
typedef enum {
    SENTRY_CRASH_STATE_READY = 0,
    SENTRY_CRASH_STATE_CRASHED = 1,
    SENTRY_CRASH_STATE_PROCESSING = 2,
    SENTRY_CRASH_STATE_CAPTURED = 3,
    SENTRY_CRASH_STATE_DONE = 4
} sentry_crash_state_t;

/**
 * Module info for minidump (captured in signal handler)
 */
typedef struct {
    uint64_t base_address;
    uint64_t size;
    char name[SENTRY_CRASH_MAX_PATH];
    uint8_t uuid[16]; // Module UUID for symbolication
    uint32_t pdb_age; // PDB age (Windows PE only, appended to debug_id)
#if defined(SENTRY_PLATFORM_WINDOWS)
    char pdb_name[SENTRY_CRASH_MAX_PATH]; // PDB filename (Windows PE only)
#endif
} sentry_module_info_t;

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)

// Max frames for pre-captured backtrace (signal handler -> daemon)
#    define SENTRY_CRASH_MAX_BACKTRACE_FRAMES 128

/**
 * Linux/Android thread context
 */
typedef struct {
    pid_t tid;
    char
        name[16]; // Thread name from /proc/[pid]/task/[tid]/comm (max 16 chars)
    ucontext_t context;
} sentry_thread_context_linux_t;

/**
 * Linux/Android specific crash context
 */
typedef struct {
    int signum;
    siginfo_t siginfo;
    ucontext_t context;

    // Pre-captured backtrace from signal handler using libunwind (DWARF-based).
    // This works without frame pointers, unlike the daemon's FP-based walking.
    // The daemon prefers this over FP-walking when backtrace_count > 0.
    size_t backtrace_count;
    uint64_t backtrace_ips[SENTRY_CRASH_MAX_BACKTRACE_FRAMES];

    // Additional thread contexts (for multi-thread dumps)
    size_t num_threads;
    sentry_thread_context_linux_t threads[SENTRY_CRASH_MAX_THREADS];
} sentry_crash_platform_linux_t;

#elif defined(SENTRY_PLATFORM_MACOS)

#    include <mach/mach.h>

/**
 * macOS thread context
 */
typedef struct {
    thread_t thread; // Mach thread port (only valid in crashed process)
    uint64_t tid; // Thread ID (portable across processes)
    _STRUCT_MCONTEXT state;
    char stack_path[SENTRY_CRASH_MAX_PATH]; // Path to saved stack memory file
    uint64_t stack_size; // Size of captured stack
    char name[64]; // Thread name from pthread_setname_np
} sentry_thread_context_darwin_t;

/**
 * macOS specific crash context
 */
typedef struct {
    int signum;
    siginfo_t siginfo;
    // Store mcontext directly (ucontext_t.uc_mcontext is just a pointer)
    _STRUCT_MCONTEXT mcontext;

    // Mach thread state
    thread_t mach_thread;

    // Additional thread contexts
    size_t num_threads;
    sentry_thread_context_darwin_t threads[SENTRY_CRASH_MAX_THREADS];
} sentry_crash_platform_darwin_t;

#elif defined(SENTRY_PLATFORM_WINDOWS)

// Disable warning C4324: structure was padded due to alignment specifier
// The Windows CONTEXT structure has alignment requirements (especially on
// ARM64) that cause padding in our wrapper structs. This is expected and
// harmless.
#    ifdef _MSC_VER
#        pragma warning(push)
#        pragma warning(disable : 4324)
#    endif

/**
 * Windows thread context
 */
typedef struct {
    DWORD thread_id;
    char name[64];
    CONTEXT context;
} sentry_thread_context_windows_t;

/**
 * Windows specific crash context
 */
typedef struct {
    DWORD exception_code;
    EXCEPTION_RECORD exception_record;
    CONTEXT context;

    // Original exception pointers in crashed process's address space
    // (needed for out-of-process minidump writing with ClientPointers=TRUE)
    EXCEPTION_POINTERS *exception_pointers;

    // Additional thread contexts
    DWORD num_threads;
    sentry_thread_context_windows_t threads[SENTRY_CRASH_MAX_THREADS];
} sentry_crash_platform_windows_t;

typedef struct {
    DWORD version;
    DWORD app_pid;
    uint64_t app_tid;
} sentry_wer_registration_t;

#    ifdef _MSC_VER
#        pragma warning(pop)
#    endif

#endif

/**
 * Shared memory structure for crash communication.
 * This MUST be safe to write from signal handlers (no allocations, no locks).
 */
typedef struct {
    // Header with magic + version for validation
    uint32_t magic;
    uint32_t version;

    // Atomic state machine (accessed via sentry__atomic_* functions)
    volatile long state;
    volatile long sequence;

    // Process info
    pid_t crashed_pid;
    pid_t crashed_tid;

    // Configuration (set by app during init)
    sentry_minidump_mode_t minidump_mode;
    int crash_reporting_mode; // sentry_crash_reporting_mode_t
    int crash_upload_mode; // sentry_crash_upload_mode_t
    bool debug_enabled; // Debug logging enabled in parent process
    bool attach_screenshot; // Screenshot attachment enabled in parent process
    bool attach_session_replay; // Session replay attachment enabled in parent
                                // process
    uint32_t session_replay_duration; // Requested session replay duration in
                                      // ms
    int cache_keep; // sentry_cache_keep_t
    bool require_user_consent;
    bool enable_large_attachments;
    bool http_retry;
    uint64_t shutdown_timeout;
    uint64_t transfer_timeout;
    bool system_crash_reporter_enabled;
    uint32_t max_breadcrumbs;

    // Atomic user consent (sentry_user_consent_t), updated whenever user
    // consent changes so the daemon can honor it at crash time.
    volatile long user_consent;

    // Platform-specific crash context
#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    sentry_crash_platform_linux_t platform;
#elif defined(SENTRY_PLATFORM_MACOS)
    sentry_crash_platform_darwin_t platform;
#elif defined(SENTRY_PLATFORM_WINDOWS)
    sentry_crash_platform_windows_t platform;
#endif

    // Sentry-specific metadata paths
    char database_path[SENTRY_CRASH_MAX_PATH]; // Database directory for all
                                               // files
    char event_path[SENTRY_CRASH_MAX_PATH];
    char breadcrumb1_path[SENTRY_CRASH_MAX_PATH];
    char breadcrumb2_path[SENTRY_CRASH_MAX_PATH];
    char envelope_path[SENTRY_CRASH_MAX_PATH];
    char external_reporter_path[SENTRY_CRASH_MAX_PATH];
    char dsn[SENTRY_CRASH_MAX_PATH]; // Sentry DSN for uploading crashes

    // Transport configuration (passed from parent options to daemon)
    char ca_certs[SENTRY_CRASH_MAX_PATH]; // CA certificates file path for SSL
    char proxy[SENTRY_CRASH_MAX_PATH]; // HTTP proxy URL
    char user_agent[256]; // User-Agent header for HTTP requests

    // Minidump output path (filled by daemon)
    char minidump_path[SENTRY_CRASH_MAX_PATH];

    // Module information (captured in signal handler from dyld)
    uint32_t module_count;
    sentry_module_info_t modules[SENTRY_CRASH_MAX_MODULES];

} sentry_crash_context_t;

// Shared memory size: calculated at compile-time based on actual struct size
// Add 8KB padding for safety and future additions
#define SENTRY_CRASH_SHM_SIZE (sizeof(sentry_crash_context_t) + (8 * 1024))

#endif
