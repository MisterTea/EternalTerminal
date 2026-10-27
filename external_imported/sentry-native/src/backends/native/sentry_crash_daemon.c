#include "sentry_crash_daemon.h"

#include "minidump/sentry_minidump_writer.h"
#include "sentry_alloc.h"
#include "sentry_attachment.h"
#include "sentry_core.h"
#include "sentry_crash_ipc.h"
#include "sentry_database.h"
#include "sentry_elf.h"
#include "sentry_envelope.h"
#include "sentry_json.h"
#include "sentry_logger.h"
#include "sentry_options.h"
#include "sentry_path.h"
#include "sentry_process.h"
#include "sentry_screenshot.h"
#include "sentry_session_replay.h"
#include "sentry_string.h"
#include "sentry_symbolizer.h"
#include "sentry_sync.h"
#include "sentry_transport.h"
#include "sentry_utils.h"
#include "sentry_uuid.h"
#include "sentry_value.h"
#include "transports/sentry_disk_transport.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(SENTRY_PLATFORM_UNIX)
#    include <dirent.h>
#    include <dlfcn.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <inttypes.h>
#    include <signal.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <sys/uio.h>
#    include <sys/wait.h>
#    include <unistd.h>
#    if defined(SENTRY_PLATFORM_MACOS)
#        include <crt_externs.h>
#        include <mach-o/dyld.h>
#        include <spawn.h>
#    endif
#    if defined(SENTRY_PLATFORM_LINUX)
#        include "unwinder/sentry_unwinder.h"
#    endif
#elif defined(SENTRY_PLATFORM_WINDOWS)
#    include <dbghelp.h>
#    include <fcntl.h>
#    include <io.h>
#    include <sys/stat.h>
#    include <windows.h>

// Forward declaration for StackWalk64-based stack unwinding (defined later)
static size_t walk_stack_with_dbghelp(HANDLE hProcess, DWORD crashed_tid,
    const CONTEXT *ctx_record, const sentry_crash_context_t *crash_ctx,
    sentry_frame_info_t *frames, size_t max_frames);
#endif

// Provide default ASAN options for sentry-crash daemon executable
// This suppresses false positives from fork() which ASAN doesn't handle well
#if defined(__has_feature)
#    if __has_feature(address_sanitizer)
const char *
__asan_default_options(void)
{
    // Disable stack-use-after-return detection which causes false positives
    // with fork+exec since ASAN's shadow memory gets confused about ownership
    return "detect_stack_use_after_return=0:halt_on_error=0";
}
#    endif
#endif

/**
 * Helper to write a file as an attachment to an envelope
 * Returns true on success, false on failure
 */
static bool
write_attachment_to_envelope(int fd, const char *file_path,
    const char *filename, const char *attachment_type, const char *content_type)
{
#if defined(SENTRY_PLATFORM_UNIX)
    int attach_fd = open(file_path, O_RDONLY);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // Use wide-char API for proper UTF-8 path support
    wchar_t *wpath = sentry__string_to_wstr(file_path);
    int attach_fd = wpath ? _wopen(wpath, _O_RDONLY | _O_BINARY) : -1;
    sentry_free(wpath);
#endif
    if (attach_fd < 0) {
        SENTRY_WARNF("Failed to open attachment file: %s", file_path);
        return false;
    }

#if defined(SENTRY_PLATFORM_UNIX)
    struct stat st;
    if (fstat(attach_fd, &st) != 0) {
        SENTRY_WARNF("Failed to stat attachment file: %s", file_path);
        close(attach_fd);
        return false;
    }
    long long file_size = (long long)st.st_size;
#elif defined(SENTRY_PLATFORM_WINDOWS)
    struct __stat64 st;
    if (_fstat64(attach_fd, &st) != 0) {
        SENTRY_WARNF("Failed to stat attachment file: %s", file_path);
        _close(attach_fd);
        return false;
    }
    long long file_size = (long long)st.st_size;
#endif

    // Write attachment item header
    sentry_jsonwriter_t *jw = sentry__jsonwriter_new_sb(NULL);
    if (!jw) {
        SENTRY_WARN("Failed to create attachment header writer");
#if defined(SENTRY_PLATFORM_UNIX)
        close(attach_fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
        _close(attach_fd);
#endif
        return false;
    }

    sentry__jsonwriter_write_object_start(jw);
    sentry__jsonwriter_write_key(jw, "type");
    sentry__jsonwriter_write_str(jw, "attachment");
    sentry__jsonwriter_write_key(jw, "length");
    sentry__jsonwriter_write_int64(jw, file_size);
    sentry__jsonwriter_write_key(jw, "attachment_type");
    sentry__jsonwriter_write_str(jw,
        sentry__string_empty(attachment_type) ? SENTRY_ATTACHMENT_TYPE_GENERIC
                                              : attachment_type);
    if (content_type) {
        sentry__jsonwriter_write_key(jw, "content_type");
        sentry__jsonwriter_write_str(jw, content_type);
    }
    sentry__jsonwriter_write_key(jw, "filename");
    sentry__jsonwriter_write_str(jw, filename ? filename : "attachment");
    sentry__jsonwriter_write_object_end(jw);

    size_t header_written = 0;
    char *header = sentry__jsonwriter_into_string(jw, &header_written);
    if (!header) {
        SENTRY_WARN("Failed to write attachment header");
#if defined(SENTRY_PLATFORM_UNIX)
        close(attach_fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
        _close(attach_fd);
#endif
        return false;
    }

#if defined(SENTRY_PLATFORM_UNIX)
    if (write(fd, header, header_written) != (ssize_t)header_written
        || write(fd, "\n", 1) != 1) {
        SENTRY_WARN("Failed to write attachment header to envelope");
    }
#elif defined(SENTRY_PLATFORM_WINDOWS)
    _write(fd, header, (unsigned int)header_written);
    _write(fd, "\n", 1);
#endif
    sentry_free(header);

    // Copy attachment content
    char buf[SENTRY_CRASH_FILE_BUFFER_SIZE];
#if defined(SENTRY_PLATFORM_UNIX)
    ssize_t n;
    while ((n = read(attach_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = write(fd, buf, n);
        if (written != n) {
            SENTRY_WARNF(
                "Failed to write attachment content for: %s", file_path);
            close(attach_fd);
            return false;
        }
    }

    if (n < 0) {
        SENTRY_WARNF("Failed to read attachment file: %s", file_path);
        close(attach_fd);
        return false;
    }

    if (write(fd, "\n", 1) != 1) {
        SENTRY_WARN("Failed to write newline to envelope");
    }
    close(attach_fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    int n;
    while ((n = _read(attach_fd, buf, sizeof(buf))) > 0) {
        int written = _write(fd, buf, (unsigned int)n);
        if (written != n) {
            SENTRY_WARNF(
                "Failed to write attachment content for: %s", file_path);
            _close(attach_fd);
            return false;
        }
    }

    if (n < 0) {
        SENTRY_WARNF("Failed to read attachment file: %s", file_path);
        _close(attach_fd);
        return false;
    }

    _write(fd, "\n", 1);
    _close(attach_fd);
#endif
    return true;
}

static bool
attachment_is_placeholder(const sentry_options_t *options, const char *path)
{
    sentry_attachment_t attachment = { 0 };
    attachment.path = sentry__path_from_str(path);
    if (!attachment.path) {
        return false;
    }
    bool is_placeholder
        = sentry__attachment_is_placeholder(&attachment, options);
    sentry__path_free(attachment.path);
    return is_placeholder;
}

// For each large attachment listed in `<run_folder>/__sentry-attachments`,
// cache it as an attachment-ref item. Small attachments were already inlined
// during envelope writing.
static void
add_attachment_refs(sentry_envelope_t *envelope,
    const sentry_options_t *options, const sentry_path_t *run_folder)
{
    if (!envelope || !options || !options->run
        || !options->enable_large_attachments || !run_folder) {
        return;
    }
    sentry_path_t *attach_list_path
        = sentry__path_join_str(run_folder, "__sentry-attachments");
    if (!attach_list_path) {
        SENTRY_WARN("Failed to resolve attachment manifest path");
        return;
    }
    size_t attach_json_len = 0;
    char *attach_json
        = sentry__path_read_to_buffer(attach_list_path, &attach_json_len);
    sentry__path_free(attach_list_path);
    if (!attach_json) {
        return;
    }
    sentry_value_t list = attach_json_len > 0
        ? sentry__value_from_json(attach_json, attach_json_len)
        : sentry_value_new_null();
    sentry_free(attach_json);
    if (sentry_value_is_null(list)) {
        SENTRY_WARN("Failed to parse attachment manifest");
        return;
    }

    bool materialized = false;
    size_t len = sentry_value_get_length(list);
    for (size_t i = 0; i < len; i++) {
        sentry_value_t info = sentry_value_get_by_index(list, i);
        const char *path
            = sentry_value_as_string(sentry_value_get_by_key(info, "path"));
        const char *filename
            = sentry_value_as_string(sentry_value_get_by_key(info, "filename"));
        const char *attachment_type = sentry_value_as_string(
            sentry_value_get_by_key(info, "attachment_type"));
        const char *content_type = sentry_value_as_string(
            sentry_value_get_by_key(info, "content_type"));
        if (sentry__string_empty(path) || sentry__string_empty(filename)) {
            SENTRY_WARN("Skipping malformed attachment manifest entry");
            continue;
        }
        sentry_attachment_t attachment = { 0 };
        attachment.path = sentry__path_from_str(path);
        attachment.filename = sentry__path_from_str(filename);
        if (!attachment.path || !attachment.filename) {
            SENTRY_WARNF("Failed to allocate attachment paths for: %s", path);
            sentry__path_free(attachment.path);
            sentry__path_free(attachment.filename);
            continue;
        }
        attachment.type
            = (char *)((attachment_type && *attachment_type) ? attachment_type
                                                             : NULL);
        attachment.content_type
            = sentry__string_empty(content_type) ? NULL : (char *)content_type;
        if (!sentry__attachment_is_placeholder(&attachment, options)) {
            sentry__path_free(attachment.path);
            sentry__path_free(attachment.filename);
            continue;
        }
        if (!materialized && !sentry__envelope_materialize(envelope)) {
            SENTRY_WARN("Failed to materialize envelope for attachment-refs");
            sentry__path_free(attachment.path);
            sentry__path_free(attachment.filename);
            break;
        }
        materialized = true;
        if (!sentry__cache_attachment_ref(
                envelope, &attachment, options->run->cache_path, NULL)) {
            SENTRY_WARN("failed to cache attachment-ref");
        }
        sentry__path_free(attachment.path);
        sentry__path_free(attachment.filename);
    }
    sentry_value_decref(list);
}

#if defined(SENTRY_PLATFORM_UNIX)
/**
 * Get signal name from signal number (Unix platforms only)
 */
static const char *
get_signal_name(int signum)
{
    switch (signum) {
    case SIGABRT:
        return "SIGABRT";
    case SIGBUS:
        return "SIGBUS";
    case SIGFPE:
        return "SIGFPE";
    case SIGILL:
        return "SIGILL";
    case SIGSEGV:
        return "SIGSEGV";
    case SIGSYS:
        return "SIGSYS";
    case SIGTRAP:
        return "SIGTRAP";
    default:
        return "UNKNOWN";
    }
}
#endif

/**
 * Build registers value from crash context for a specific thread.
 *
 * @param ctx The crash context
 * @param thread_idx Index of the thread in ctx->platform.threads[]
 *                   Pass SIZE_MAX to use the crashed thread context
 * @return Registers value object
 */
static sentry_value_t
build_registers_from_ctx(const sentry_crash_context_t *ctx, size_t thread_idx)
{
    sentry_value_t registers = sentry_value_new_object();

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    // Use thread-specific context, defaulting to crashed thread
    const ucontext_t *uctx = &ctx->platform.context;
    if (thread_idx != SIZE_MAX && ctx->platform.num_threads > 0
        && thread_idx < ctx->platform.num_threads) {
        uctx = &ctx->platform.threads[thread_idx].context;
    }

#    if defined(__x86_64__)
    uintptr_t *mctx = (uintptr_t *)&uctx->uc_mcontext;
    sentry_value_set_by_key(
        registers, "r8", sentry__value_new_addr((uint64_t)mctx[0]));
    sentry_value_set_by_key(
        registers, "r9", sentry__value_new_addr((uint64_t)mctx[1]));
    sentry_value_set_by_key(
        registers, "r10", sentry__value_new_addr((uint64_t)mctx[2]));
    sentry_value_set_by_key(
        registers, "r11", sentry__value_new_addr((uint64_t)mctx[3]));
    sentry_value_set_by_key(
        registers, "r12", sentry__value_new_addr((uint64_t)mctx[4]));
    sentry_value_set_by_key(
        registers, "r13", sentry__value_new_addr((uint64_t)mctx[5]));
    sentry_value_set_by_key(
        registers, "r14", sentry__value_new_addr((uint64_t)mctx[6]));
    sentry_value_set_by_key(
        registers, "r15", sentry__value_new_addr((uint64_t)mctx[7]));
    sentry_value_set_by_key(
        registers, "rdi", sentry__value_new_addr((uint64_t)mctx[8]));
    sentry_value_set_by_key(
        registers, "rsi", sentry__value_new_addr((uint64_t)mctx[9]));
    sentry_value_set_by_key(
        registers, "rbp", sentry__value_new_addr((uint64_t)mctx[10]));
    sentry_value_set_by_key(
        registers, "rbx", sentry__value_new_addr((uint64_t)mctx[11]));
    sentry_value_set_by_key(
        registers, "rdx", sentry__value_new_addr((uint64_t)mctx[12]));
    sentry_value_set_by_key(
        registers, "rax", sentry__value_new_addr((uint64_t)mctx[13]));
    sentry_value_set_by_key(
        registers, "rcx", sentry__value_new_addr((uint64_t)mctx[14]));
    sentry_value_set_by_key(
        registers, "rsp", sentry__value_new_addr((uint64_t)mctx[15]));
    sentry_value_set_by_key(
        registers, "rip", sentry__value_new_addr((uint64_t)mctx[16]));
#    elif defined(__aarch64__)
    // Use struct field access instead of raw pointer indexing because
    // struct sigcontext has fault_address before regs[31] on aarch64.
    for (int i = 0; i < 29; i++) {
        char name[4];
        snprintf(name, sizeof(name), "x%d", i);
        sentry_value_set_by_key(
            registers, name, sentry__value_new_addr(uctx->uc_mcontext.regs[i]));
    }
    sentry_value_set_by_key(
        registers, "fp", sentry__value_new_addr(uctx->uc_mcontext.regs[29]));
    sentry_value_set_by_key(
        registers, "lr", sentry__value_new_addr(uctx->uc_mcontext.regs[30]));
    sentry_value_set_by_key(
        registers, "sp", sentry__value_new_addr(uctx->uc_mcontext.sp));
    sentry_value_set_by_key(
        registers, "pc", sentry__value_new_addr(uctx->uc_mcontext.pc));
#    endif

#elif defined(SENTRY_PLATFORM_MACOS)
    // Use thread-specific context, defaulting to crashed thread
    const _STRUCT_MCONTEXT *mctx = &ctx->platform.mcontext;
    if (thread_idx != SIZE_MAX && ctx->platform.num_threads > 0
        && thread_idx < ctx->platform.num_threads) {
        mctx = &ctx->platform.threads[thread_idx].state;
    }

#    if defined(__x86_64__)
    sentry_value_set_by_key(
        registers, "rax", sentry__value_new_addr(mctx->__ss.__rax));
    sentry_value_set_by_key(
        registers, "rbx", sentry__value_new_addr(mctx->__ss.__rbx));
    sentry_value_set_by_key(
        registers, "rcx", sentry__value_new_addr(mctx->__ss.__rcx));
    sentry_value_set_by_key(
        registers, "rdx", sentry__value_new_addr(mctx->__ss.__rdx));
    sentry_value_set_by_key(
        registers, "rdi", sentry__value_new_addr(mctx->__ss.__rdi));
    sentry_value_set_by_key(
        registers, "rsi", sentry__value_new_addr(mctx->__ss.__rsi));
    sentry_value_set_by_key(
        registers, "rbp", sentry__value_new_addr(mctx->__ss.__rbp));
    sentry_value_set_by_key(
        registers, "rsp", sentry__value_new_addr(mctx->__ss.__rsp));
    sentry_value_set_by_key(
        registers, "r8", sentry__value_new_addr(mctx->__ss.__r8));
    sentry_value_set_by_key(
        registers, "r9", sentry__value_new_addr(mctx->__ss.__r9));
    sentry_value_set_by_key(
        registers, "r10", sentry__value_new_addr(mctx->__ss.__r10));
    sentry_value_set_by_key(
        registers, "r11", sentry__value_new_addr(mctx->__ss.__r11));
    sentry_value_set_by_key(
        registers, "r12", sentry__value_new_addr(mctx->__ss.__r12));
    sentry_value_set_by_key(
        registers, "r13", sentry__value_new_addr(mctx->__ss.__r13));
    sentry_value_set_by_key(
        registers, "r14", sentry__value_new_addr(mctx->__ss.__r14));
    sentry_value_set_by_key(
        registers, "r15", sentry__value_new_addr(mctx->__ss.__r15));
    sentry_value_set_by_key(
        registers, "rip", sentry__value_new_addr(mctx->__ss.__rip));
#    elif defined(__aarch64__)
    for (int i = 0; i < 29; i++) {
        char name[4];
        snprintf(name, sizeof(name), "x%d", i);
        sentry_value_set_by_key(
            registers, name, sentry__value_new_addr(mctx->__ss.__x[i]));
    }
    sentry_value_set_by_key(registers, "fp",
        sentry__value_new_addr(SENTRY__ARM64_GET_FP(mctx->__ss)));
    sentry_value_set_by_key(registers, "lr",
        sentry__value_new_addr(SENTRY__ARM64_GET_LR(mctx->__ss)));
    sentry_value_set_by_key(registers, "sp",
        sentry__value_new_addr(SENTRY__ARM64_GET_SP(mctx->__ss)));
    sentry_value_set_by_key(registers, "pc",
        sentry__value_new_addr(SENTRY__ARM64_GET_PC(mctx->__ss)));
#    endif

#elif defined(SENTRY_PLATFORM_WINDOWS)
    // Use thread-specific context, defaulting to crashed thread
    const CONTEXT *wctx = &ctx->platform.context;
    if (thread_idx != SIZE_MAX && ctx->platform.num_threads > 0
        && thread_idx < ctx->platform.num_threads) {
        wctx = &ctx->platform.threads[thread_idx].context;
    }

#    if defined(_M_AMD64)
    sentry_value_set_by_key(
        registers, "rax", sentry__value_new_addr(wctx->Rax));
    sentry_value_set_by_key(
        registers, "rbx", sentry__value_new_addr(wctx->Rbx));
    sentry_value_set_by_key(
        registers, "rcx", sentry__value_new_addr(wctx->Rcx));
    sentry_value_set_by_key(
        registers, "rdx", sentry__value_new_addr(wctx->Rdx));
    sentry_value_set_by_key(
        registers, "rdi", sentry__value_new_addr(wctx->Rdi));
    sentry_value_set_by_key(
        registers, "rsi", sentry__value_new_addr(wctx->Rsi));
    sentry_value_set_by_key(
        registers, "rbp", sentry__value_new_addr(wctx->Rbp));
    sentry_value_set_by_key(
        registers, "rsp", sentry__value_new_addr(wctx->Rsp));
    sentry_value_set_by_key(registers, "r8", sentry__value_new_addr(wctx->R8));
    sentry_value_set_by_key(registers, "r9", sentry__value_new_addr(wctx->R9));
    sentry_value_set_by_key(
        registers, "r10", sentry__value_new_addr(wctx->R10));
    sentry_value_set_by_key(
        registers, "r11", sentry__value_new_addr(wctx->R11));
    sentry_value_set_by_key(
        registers, "r12", sentry__value_new_addr(wctx->R12));
    sentry_value_set_by_key(
        registers, "r13", sentry__value_new_addr(wctx->R13));
    sentry_value_set_by_key(
        registers, "r14", sentry__value_new_addr(wctx->R14));
    sentry_value_set_by_key(
        registers, "r15", sentry__value_new_addr(wctx->R15));
    sentry_value_set_by_key(
        registers, "rip", sentry__value_new_addr(wctx->Rip));
#    elif defined(_M_IX86)
    sentry_value_set_by_key(
        registers, "eax", sentry__value_new_addr(wctx->Eax));
    sentry_value_set_by_key(
        registers, "ebx", sentry__value_new_addr(wctx->Ebx));
    sentry_value_set_by_key(
        registers, "ecx", sentry__value_new_addr(wctx->Ecx));
    sentry_value_set_by_key(
        registers, "edx", sentry__value_new_addr(wctx->Edx));
    sentry_value_set_by_key(
        registers, "edi", sentry__value_new_addr(wctx->Edi));
    sentry_value_set_by_key(
        registers, "esi", sentry__value_new_addr(wctx->Esi));
    sentry_value_set_by_key(
        registers, "ebp", sentry__value_new_addr(wctx->Ebp));
    sentry_value_set_by_key(
        registers, "esp", sentry__value_new_addr(wctx->Esp));
    sentry_value_set_by_key(
        registers, "eip", sentry__value_new_addr(wctx->Eip));
#    elif defined(_M_ARM64)
    for (int i = 0; i < 29; i++) {
        char name[4];
        snprintf(name, sizeof(name), "x%d", i);
        sentry_value_set_by_key(
            registers, name, sentry__value_new_addr(wctx->X[i]));
    }
    sentry_value_set_by_key(registers, "fp", sentry__value_new_addr(wctx->Fp));
    sentry_value_set_by_key(registers, "lr", sentry__value_new_addr(wctx->Lr));
    sentry_value_set_by_key(registers, "sp", sentry__value_new_addr(wctx->Sp));
    sentry_value_set_by_key(registers, "pc", sentry__value_new_addr(wctx->Pc));
#    endif
#endif

    return registers;
}

#ifdef SENTRY_WITH_UNWINDER_LIBUNWIND_REMOTE
static sentry_value_t
build_registers_from_remote_registers(
    const sentry_remote_registers_t *remote_registers)
{
    sentry_value_t registers = sentry_value_new_object();

    for (size_t i = 0; i < remote_registers->count; i++) {
        const sentry_remote_register_t *remote_register
            = &remote_registers->values[i];
        sentry_value_set_by_key(registers, remote_register->name,
            sentry__value_new_addr(remote_register->value));
    }

    return registers;
}
#endif

/**
 * Maximum number of frames to unwind
 */
#define MAX_STACK_FRAMES 128

/**
 * Read a pointer-sized value from the stack buffer.
 * Returns true if successful, false if address is outside the buffer.
 */
static bool
read_stack_value(const uint8_t *stack_buf, uint64_t stack_start,
    uint64_t stack_size, uint64_t addr, uint64_t *out_value)
{
    if (addr < stack_start
        || addr + sizeof(uint64_t) > stack_start + stack_size) {
        return false;
    }
    uint64_t offset = addr - stack_start;
    memcpy(out_value, stack_buf + offset, sizeof(uint64_t));
    return true;
}

/**
 * Check if an address looks like a valid code pointer.
 * Basic sanity check to avoid garbage in the stacktrace.
 */
static bool
is_valid_code_addr(uint64_t addr)
{
    // Must be non-null and in typical code range
    if (addr == 0 || addr < 0x1000) {
        return false;
    }
#if defined(__x86_64__) || defined(_M_AMD64)
    // On x86_64, user space is below the canonical address boundary
    if (addr > 0x00007FFFFFFFFFFF) {
        return false;
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    // On ARM64 with 48-bit VA, user space is typically 0x0 to 0xFFFF_FFFF_FFFF
    // Kernel space starts at 0xFFFF_0000_0000_0000
    // Addresses like 0xAAAA_xxxx are valid user space addresses with ASLR
    if (addr >= 0xFFFF000000000000ULL) {
        return false; // Kernel space
    }
#endif
    return true;
}

/**
 * Find the module containing the given address and add module info to frame.
 * Sets 'package' (module name) and 'image_addr' on the frame if found.
 *
 * @param ctx The crash context containing module list
 * @param frame The frame value to enrich
 * @param addr The instruction address to look up
 */
static void
enrich_frame_with_module_info(
    const sentry_crash_context_t *ctx, sentry_value_t frame, uint64_t addr)
{
    uint32_t module_count = MIN(ctx->module_count, SENTRY_CRASH_MAX_MODULES);
    for (uint32_t i = 0; i < module_count; i++) {
        const sentry_module_info_t *mod = &ctx->modules[i];
        if (addr >= mod->base_address && addr < mod->base_address + mod->size) {
            // Set package to full module path (matches minidump format)
            sentry_value_set_by_key(
                frame, "package", sentry_value_new_string(mod->name));
            // Note: Do NOT set image_addr on frames - it's not present in
            // minidump-derived events and may cause symbolicator issues
            SENTRY_DEBUGF("Frame 0x%llx -> module %s", (unsigned long long)addr,
                mod->name);
            return;
        }
    }
    // No matching module found - log for debugging
    SENTRY_DEBUGF("Frame 0x%llx NOT matched to any module (module_count=%u)",
        (unsigned long long)addr, module_count);
}

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
static void enrich_frame_with_symbol(
    const sentry_crash_context_t *ctx, sentry_value_t frame, uint64_t addr);
#endif

/**
 * Build stacktrace frames for a specific thread using frame pointer-based
 * unwinding. Reads the captured stack memory and walks the frame chain.
 *
 * @param ctx The crash context
 * @param thread_idx Index of the thread in ctx->platform.threads[]
 *                   Pass SIZE_MAX to use the crashed thread from mcontext
 * @return Stacktrace value with frames array
 */
static sentry_value_t
build_stacktrace_for_thread(
    const sentry_crash_context_t *ctx, size_t thread_idx)
{
    sentry_value_t stacktrace = sentry_value_new_object();
    sentry_value_t frames = sentry_value_new_list();

    // Suppress unused parameter warning on platforms where thread_idx isn't
    // used
    (void)thread_idx;

    // Get instruction pointer and frame pointer from crash context
    uint64_t ip = 0;
    uint64_t fp = 0;
    uint64_t sp = 0;
#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    // Use thread-specific context, defaulting to crashed thread
    const ucontext_t *thread_context = &ctx->platform.context;
    if (thread_idx != SIZE_MAX && ctx->platform.num_threads > 0
        && thread_idx < ctx->platform.num_threads) {
        thread_context = &ctx->platform.threads[thread_idx].context;
    }

#    if defined(__x86_64__)
    ip = (uint64_t)thread_context->uc_mcontext.gregs[REG_RIP];
    fp = (uint64_t)thread_context->uc_mcontext.gregs[REG_RBP];
    sp = (uint64_t)thread_context->uc_mcontext.gregs[REG_RSP];
#    elif defined(__aarch64__)
    ip = (uint64_t)thread_context->uc_mcontext.pc;
    fp = (uint64_t)thread_context->uc_mcontext.regs[29]; // x29 is FP
    sp = (uint64_t)thread_context->uc_mcontext.sp;
#    elif defined(__i386__)
    ip = (uint64_t)thread_context->uc_mcontext.gregs[REG_EIP];
    fp = (uint64_t)thread_context->uc_mcontext.gregs[REG_EBP];
    sp = (uint64_t)thread_context->uc_mcontext.gregs[REG_ESP];
#    elif defined(__arm__)
    ip = (uint64_t)thread_context->uc_mcontext.arm_pc;
    fp = (uint64_t)thread_context->uc_mcontext.arm_fp;
    sp = (uint64_t)thread_context->uc_mcontext.arm_sp;
#    endif
#elif defined(SENTRY_PLATFORM_MACOS)
#    if defined(__x86_64__)
    ip = ctx->platform.mcontext.__ss.__rip;
    fp = ctx->platform.mcontext.__ss.__rbp;
    sp = ctx->platform.mcontext.__ss.__rsp;
#    elif defined(__aarch64__)
    ip = SENTRY__ARM64_GET_PC(ctx->platform.mcontext.__ss);
    fp = SENTRY__ARM64_GET_FP(ctx->platform.mcontext.__ss);
    sp = SENTRY__ARM64_GET_SP(ctx->platform.mcontext.__ss);
#    endif
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // Use thread-specific context, defaulting to crashed thread
    const CONTEXT *thread_context = &ctx->platform.context;
    if (thread_idx != SIZE_MAX && ctx->platform.num_threads > 0
        && thread_idx < ctx->platform.num_threads) {
        thread_context = &ctx->platform.threads[thread_idx].context;
    }

#    if defined(_M_AMD64)
    ip = thread_context->Rip;
    fp = thread_context->Rbp;
    sp = thread_context->Rsp;
#    elif defined(_M_IX86)
    ip = thread_context->Eip;
    fp = thread_context->Ebp;
    sp = thread_context->Esp;
#    elif defined(_M_ARM64)
    ip = thread_context->Pc;
    fp = thread_context->Fp;
    sp = thread_context->Sp;
#    endif
#endif

    (void)sp; // May be unused depending on platform

    // Try to read stack memory from the captured stack file or process memory
    uint8_t *stack_buf = NULL;
    uint64_t stack_start = 0;
    uint64_t stack_size = 0;

#if defined(SENTRY_PLATFORM_MACOS)
    // On macOS, stack is saved to a file by the signal handler.
    // Use the specified thread index, or find the crashed thread if SIZE_MAX.
    if (ctx->platform.num_threads > 0) {
        size_t idx = thread_idx;

        // If SIZE_MAX, find the crashed thread
        if (idx == SIZE_MAX) {
            idx = 0;
            for (size_t i = 0; i < ctx->platform.num_threads; i++) {
                if (ctx->platform.threads[i].tid
                    == (uint64_t)ctx->crashed_tid) {
                    idx = i;
                    break;
                }
            }
        }

        // Validate index
        if (idx >= ctx->platform.num_threads) {
            SENTRY_WARNF("Invalid thread index %zu (max %zu)", idx,
                ctx->platform.num_threads);
            sentry_value_set_by_key(stacktrace, "frames", frames);
            return stacktrace;
        }

        const sentry_thread_context_darwin_t *thread
            = &ctx->platform.threads[idx];

        // Use IP/FP/SP from the thread state (matches saved stack)
#    if defined(__x86_64__)
        ip = thread->state.__ss.__rip;
        fp = thread->state.__ss.__rbp;
        sp = thread->state.__ss.__rsp;
#    elif defined(__aarch64__)
        ip = SENTRY__ARM64_GET_PC(thread->state.__ss);
        fp = SENTRY__ARM64_GET_FP(thread->state.__ss);
        sp = SENTRY__ARM64_GET_SP(thread->state.__ss);
#    endif

        SENTRY_DEBUGF("Thread %zu: IP=0x%llx FP=0x%llx SP=0x%llx", idx,
            (unsigned long long)ip, (unsigned long long)fp,
            (unsigned long long)sp);

        const char *stack_path = thread->stack_path;
        stack_size = thread->stack_size;
        if (stack_path[0] != '\0' && stack_size > 0) {
            int stack_fd = open(stack_path, O_RDONLY);
            if (stack_fd >= 0) {
                stack_buf = sentry_malloc(stack_size);
                if (stack_buf) {
                    ssize_t bytes_read = read(stack_fd, stack_buf, stack_size);
                    if (bytes_read == (ssize_t)stack_size) {
                        // Stack was captured from SP upward
                        stack_start = sp;
                        SENTRY_DEBUGF(
                            "Loaded stack: start=0x%llx size=%llu, FP offset "
                            "from SP=%lld",
                            (unsigned long long)stack_start,
                            (unsigned long long)stack_size,
                            (long long)(fp - sp));
                    } else {
                        SENTRY_WARNF(
                            "Stack read failed: got %zd, expected %llu",
                            bytes_read, (unsigned long long)stack_size);
                        sentry_free(stack_buf);
                        stack_buf = NULL;
                    }
                }
                close(stack_fd);
            } else {
                SENTRY_WARNF("Failed to open stack file: %s", stack_path);
            }
        } else {
            SENTRY_DEBUGF("No stack file for thread %zu", idx);
        }
    }
#elif defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    // On Linux, use process_vm_readv to read stack memory from crashed process
    if (ctx->platform.num_threads > 0) {
        pid_t pid = ctx->crashed_pid;
        stack_size = SENTRY_CRASH_MAX_STACK_CAPTURE;
        stack_start = sp;
        stack_buf = sentry_malloc(stack_size);
        if (stack_buf) {
            struct iovec local_iov
                = { .iov_base = stack_buf, .iov_len = stack_size };
            struct iovec remote_iov
                = { .iov_base = (void *)stack_start, .iov_len = stack_size };
            ssize_t bytes_read
                = process_vm_readv(pid, &local_iov, 1, &remote_iov, 1, 0);
            if (bytes_read <= 0) {
                SENTRY_DEBUG(
                    "process_vm_readv failed, falling back to single frame");
                sentry_free(stack_buf);
                stack_buf = NULL;
            } else {
                stack_size = (uint64_t)bytes_read;
                SENTRY_DEBUGF(
                    "Read %zd bytes of stack from process %d", bytes_read, pid);
            }
        }
    }
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // On Windows, use StackWalk64 for proper stack unwinding
    // This uses PE unwind info and works reliably on x64/ARM64

    // Get thread-specific context for stack walking
    const CONTEXT *walk_context = &ctx->platform.context;
    DWORD walk_thread_id = (DWORD)ctx->crashed_tid;
    if (thread_idx != SIZE_MAX && ctx->platform.num_threads > 0
        && thread_idx < ctx->platform.num_threads) {
        const sentry_thread_context_windows_t *tctx
            = &ctx->platform.threads[thread_idx];
        walk_context = &tctx->context;
        walk_thread_id = tctx->thread_id;
    }

    HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, (DWORD)ctx->crashed_pid);
    if (hProcess) {
        sentry_frame_info_t stack_frames[MAX_STACK_FRAMES];
        // Make a copy since StackWalk64 may modify the context
        CONTEXT ctx_copy = *walk_context;
        size_t dbghelp_frame_count = walk_stack_with_dbghelp(hProcess,
            walk_thread_id, &ctx_copy, ctx, stack_frames, MAX_STACK_FRAMES);

        if (dbghelp_frame_count > 0) {
            // Build sentry frames from StackWalk64 results
            sentry_value_t temp_frames[MAX_STACK_FRAMES];
            int frame_count = 0;

            for (size_t i = 0;
                i < dbghelp_frame_count && frame_count < MAX_STACK_FRAMES;
                i++) {
                uint64_t frame_addr
                    = (uint64_t)(uintptr_t)stack_frames[i].instruction_addr;
                temp_frames[frame_count] = sentry_value_new_object();
                sentry_value_set_by_key(temp_frames[frame_count],
                    "instruction_addr", sentry__value_new_addr(frame_addr));
                // First frame is from context, rest are from CFI unwinding
                sentry_value_set_by_key(temp_frames[frame_count], "trust",
                    sentry_value_new_string(i == 0 ? "context" : "cfi"));
                enrich_frame_with_module_info(
                    ctx, temp_frames[frame_count], frame_addr);
                if (stack_frames[i].symbol) {
                    sentry_value_set_by_key(temp_frames[frame_count],
                        "function",
                        sentry_value_new_string(stack_frames[i].symbol));
                }
                if (stack_frames[i].symbol_addr) {
                    sentry_value_set_by_key(temp_frames[frame_count],
                        "symbol_addr",
                        sentry__value_new_addr(
                            (uint64_t)(uintptr_t)stack_frames[i].symbol_addr));
                }
                frame_count++;
            }

            for (size_t i = 0; i < dbghelp_frame_count; i++) {
                sentry_free((char *)stack_frames[i].symbol);
            }

            // Sentry expects frames in reverse order (outermost caller first)
            for (int i = frame_count - 1; i >= 0; i--) {
                sentry_value_append(frames, temp_frames[i]);
            }

            sentry_value_set_by_key(stacktrace, "frames", frames);
            sentry_value_set_by_key(stacktrace, "registers",
                build_registers_from_ctx(ctx, thread_idx));

            CloseHandle(hProcess);
            return stacktrace;
        }

        CloseHandle(hProcess);
    } else {
        SENTRY_WARNF("Failed to open process %d for stack walk (error %lu)",
            ctx->crashed_pid, GetLastError());
    }
    // Fall through to add at least the IP frame below
#endif

    // Build frame list - collect in callee-first order, then reverse for Sentry
    sentry_value_t temp_frames[MAX_STACK_FRAMES];
    int frame_count = 0;

#if defined(SENTRY_PLATFORM_LINUX)
    // Remote DWARF unwinding via libunwind ptrace accessors. Do not attach to
    // the crashed thread from the daemon; use the saved fault context below.
    {
#    ifdef SENTRY_WITH_UNWINDER_LIBUNWIND_REMOTE
        pid_t tid = 0;
        if (thread_idx == SIZE_MAX) {
            tid = ctx->crashed_tid;
        } else if (thread_idx < ctx->platform.num_threads) {
            tid = ctx->platform.threads[thread_idx].tid;
        }

        bool is_crashed_thread
            = thread_idx == SIZE_MAX || tid == ctx->crashed_tid;

        if (tid > 0 && !is_crashed_thread) {
            sentry_remote_registers_t registers = { 0 };
            sentry_remote_frame_t *remote_frames
                = sentry_malloc(sizeof(*remote_frames) * MAX_STACK_FRAMES);
            size_t remote_count = remote_frames
                ? sentry__unwind_stack_from_thread(
                      tid, remote_frames, MAX_STACK_FRAMES, &registers)
                : 0;

            if (remote_count > 0) {
                SENTRY_DEBUGF("Remote unwound %zu frames for thread %d",
                    remote_count, tid);

                for (size_t i = 0;
                    i < remote_count && frame_count < MAX_STACK_FRAMES; i++) {
                    if (remote_frames[i].ip == 0
                        || !is_valid_code_addr(remote_frames[i].ip)) {
                        continue;
                    }
                    temp_frames[frame_count] = sentry_value_new_object();
                    sentry_value_set_by_key(temp_frames[frame_count],
                        "instruction_addr",
                        sentry__value_new_addr(remote_frames[i].ip));
                    // Trust describes the unwind source, not the emitted
                    // frame index. If the initial cursor frame is filtered
                    // out, the next emitted frame was still reached via CFI.
                    sentry_value_set_by_key(temp_frames[frame_count], "trust",
                        sentry_value_new_string(i == 0 ? "context" : "cfi"));
                    enrich_frame_with_module_info(
                        ctx, temp_frames[frame_count], remote_frames[i].ip);
                    if (remote_frames[i].symbol[0]) {
                        sentry_value_set_by_key(temp_frames[frame_count],
                            "function",
                            sentry_value_new_string(remote_frames[i].symbol));
                        if (remote_frames[i].symbol_offset > 0
                            && remote_frames[i].symbol_offset
                                <= remote_frames[i].ip) {
                            sentry_value_set_by_key(temp_frames[frame_count],
                                "symbol_addr",
                                sentry__value_new_addr(remote_frames[i].ip
                                    - remote_frames[i].symbol_offset));
                        }
                    }
                    frame_count++;
                }

                if (frame_count > 0) {
                    if (stack_buf) {
                        sentry_free(stack_buf);
                    }

                    for (int i = frame_count - 1; i >= 0; i--) {
                        sentry_value_append(frames, temp_frames[i]);
                    }
                    sentry_value_set_by_key(stacktrace, "frames", frames);
                    if (registers.count > 0) {
                        sentry_value_set_by_key(stacktrace, "registers",
                            build_registers_from_remote_registers(&registers));
                    }
                    sentry_free(remote_frames);
                    return stacktrace;
                }
            }

            if (remote_frames) {
                sentry_free(remote_frames);
            }
        }
#    endif
    }
    // Fall through to pre-captured backtrace or FP-walking if remote
    // unwinding failed
#endif

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    // Fallback: use pre-captured libunwind backtrace if available
    // (DWARF-based, works without frame pointers).
    if (ctx->platform.backtrace_count > 0
        && (thread_idx == SIZE_MAX || thread_idx == 0)) {
        SENTRY_DEBUGF("Using pre-captured libunwind backtrace (%zu frames)",
            ctx->platform.backtrace_count);

        for (size_t i = 0;
            i < ctx->platform.backtrace_count && frame_count < MAX_STACK_FRAMES;
            i++) {
            uint64_t frame_ip = ctx->platform.backtrace_ips[i];
            if (frame_ip == 0 || !is_valid_code_addr(frame_ip)) {
                continue;
            }
            temp_frames[frame_count] = sentry_value_new_object();
            sentry_value_set_by_key(temp_frames[frame_count],
                "instruction_addr", sentry__value_new_addr(frame_ip));
            // Trust describes the unwind source, not the emitted frame index.
            // If the initial cursor frame is filtered out, the next emitted
            // frame was still reached via CFI.
            sentry_value_set_by_key(temp_frames[frame_count], "trust",
                sentry_value_new_string(i == 0 ? "context" : "cfi"));
            enrich_frame_with_module_info(
                ctx, temp_frames[frame_count], frame_ip);
            enrich_frame_with_symbol(ctx, temp_frames[frame_count], frame_ip);
            frame_count++;
        }

        if (stack_buf) {
            sentry_free(stack_buf);
        }

        if (frame_count == 0) {
            sentry_value_decref(frames);
            sentry_value_decref(stacktrace);
            return sentry_value_new_null();
        }

        // Sentry expects frames in reverse order (outermost caller first)
        for (int i = frame_count - 1; i >= 0; i--) {
            sentry_value_append(frames, temp_frames[i]);
        }

        sentry_value_set_by_key(stacktrace, "frames", frames);
        sentry_value_set_by_key(
            stacktrace, "registers", build_registers_from_ctx(ctx, thread_idx));
        return stacktrace;
    }
#endif

    // Add the crashing frame (instruction pointer)
    if (ip != 0 && is_valid_code_addr(ip)) {
        temp_frames[frame_count] = sentry_value_new_object();
        sentry_value_set_by_key(temp_frames[frame_count], "instruction_addr",
            sentry__value_new_addr(ip));
        // Trust "context" = from CPU context (the crashing frame)
        sentry_value_set_by_key(temp_frames[frame_count], "trust",
            sentry_value_new_string("context"));
        enrich_frame_with_module_info(ctx, temp_frames[frame_count], ip);
#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
        enrich_frame_with_symbol(ctx, temp_frames[frame_count], ip);
#endif
        frame_count++;
    }

    // Walk the frame pointer chain if we have stack memory
    if (stack_buf && fp != 0 && frame_count < MAX_STACK_FRAMES) {
        uint64_t current_fp = fp;
        int walk_count = 0;

        // Check if FP is within captured stack range
        uint64_t stack_end = stack_start + stack_size;
        if (current_fp < stack_start || current_fp >= stack_end) {
            SENTRY_WARNF("FP 0x%llx outside captured stack [0x%llx - 0x%llx]",
                (unsigned long long)current_fp, (unsigned long long)stack_start,
                (unsigned long long)stack_end);
        }

        while (walk_count < MAX_STACK_FRAMES - frame_count) {
            uint64_t saved_fp = 0;
            uint64_t return_addr = 0;

            // Read saved frame pointer and return address
            // Frame layout: [FP+0] = saved FP, [FP+8] = return addr
            if (!read_stack_value(stack_buf, stack_start, stack_size,
                    current_fp, &saved_fp)) {
                SENTRY_DEBUGF(
                    "Cannot read saved FP at 0x%llx (stack: 0x%llx - 0x%llx)",
                    (unsigned long long)current_fp,
                    (unsigned long long)stack_start,
                    (unsigned long long)stack_end);
                break;
            }
            if (!read_stack_value(stack_buf, stack_start, stack_size,
                    current_fp + sizeof(uint64_t), &return_addr)) {
                SENTRY_DEBUGF("Cannot read return addr at 0x%llx",
                    (unsigned long long)(current_fp + sizeof(uint64_t)));
                break;
            }

            SENTRY_DEBUGF("Frame %d: FP=0x%llx saved_fp=0x%llx ret=0x%llx",
                walk_count, (unsigned long long)current_fp,
                (unsigned long long)saved_fp, (unsigned long long)return_addr);

            // Validate the return address
            if (!is_valid_code_addr(return_addr)) {
                SENTRY_DEBUGF("Invalid return addr 0x%llx",
                    (unsigned long long)return_addr);
                break;
            }

            // Add frame
            temp_frames[frame_count] = sentry_value_new_object();
            sentry_value_set_by_key(temp_frames[frame_count],
                "instruction_addr", sentry__value_new_addr(return_addr));
            // Trust "fp" = frame pointer based unwinding
            sentry_value_set_by_key(temp_frames[frame_count], "trust",
                sentry_value_new_string("fp"));
            enrich_frame_with_module_info(
                ctx, temp_frames[frame_count], return_addr);
#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
            enrich_frame_with_symbol(
                ctx, temp_frames[frame_count], return_addr);
#endif
            frame_count++;
            walk_count++;

            // Check for end of chain
            if (saved_fp == 0 || saved_fp == current_fp) {
                SENTRY_DEBUGF(
                    "End of frame chain at FP=0x%llx (saved_fp=0x%llx)",
                    (unsigned long long)current_fp,
                    (unsigned long long)saved_fp);
                break;
            }

            // Sanity check: frame pointer should increase (stack grows down)
            if (saved_fp < current_fp) {
                SENTRY_DEBUGF("FP went backwards: 0x%llx -> 0x%llx",
                    (unsigned long long)current_fp,
                    (unsigned long long)saved_fp);
                break;
            }

            current_fp = saved_fp;
        }

        SENTRY_DEBUGF("Unwound %d frames total", frame_count);
    }

    // Free stack buffer
    if (stack_buf) {
        sentry_free(stack_buf);
    }

    // If no frames, return null (Sentry rejects empty frames arrays)
    if (frame_count == 0) {
        sentry_value_decref(frames);
        sentry_value_decref(stacktrace);
        return sentry_value_new_null();
    }

    // Sentry expects frames in reverse order (outermost caller first)
    for (int i = frame_count - 1; i >= 0; i--) {
        sentry_value_append(frames, temp_frames[i]);
    }

    sentry_value_set_by_key(stacktrace, "frames", frames);
    sentry_value_set_by_key(
        stacktrace, "registers", build_registers_from_ctx(ctx, thread_idx));

    return stacktrace;
}

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
#    include "sentry_elf.h"
#    include <elf.h>

/**
 * Extract Build ID from ELF file (for debug_meta)
 * Returns the Build ID length, or 0 if not found
 */
static size_t
extract_elf_build_id_for_module(
    const char *elf_path, uint8_t *build_id, size_t max_len)
{
    int fd = open(elf_path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    // Read ELF header
#    if defined(__x86_64__) || defined(__aarch64__)
    Elf64_Ehdr ehdr;
#    else
    Elf32_Ehdr ehdr;
#    endif

    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        close(fd);
        return 0;
    }

    // Verify ELF magic
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        close(fd);
        return 0;
    }

    if (!sentry__elf_has_shdr_size(ehdr.e_ident, ehdr.e_shentsize)) {
        close(fd);
        return 0;
    }

    // Read section headers
    size_t shdr_size = (size_t)ehdr.e_shentsize * ehdr.e_shnum;
    void *shdr_buf = sentry_malloc(shdr_size);
    if (!shdr_buf) {
        close(fd);
        return 0;
    }

    if (lseek(fd, ehdr.e_shoff, SEEK_SET) != (off_t)ehdr.e_shoff
        || read(fd, shdr_buf, shdr_size) != (ssize_t)shdr_size) {
        sentry_free(shdr_buf);
        close(fd);
        return 0;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    Elf64_Shdr *sections = (Elf64_Shdr *)shdr_buf;
#    else
    Elf32_Shdr *sections = (Elf32_Shdr *)shdr_buf;
#    endif

    // Look for .note.gnu.build-id section
    size_t build_id_len = 0;
    for (int i = 0; i < ehdr.e_shnum; i++) {
        if (sections[i].sh_type != SHT_NOTE) {
            continue;
        }

        size_t note_size = sections[i].sh_size;
        if (note_size > 4096) {
            continue; // Sanity check
        }

        void *note_buf = sentry_malloc(note_size);
        if (!note_buf) {
            continue;
        }

        if (lseek(fd, sections[i].sh_offset, SEEK_SET)
                != (off_t)sections[i].sh_offset
            || read(fd, note_buf, note_size) != (ssize_t)note_size) {
            sentry_free(note_buf);
            continue;
        }

        size_t desc_size;
        const void *desc = sentry__elf_find_note(
            note_buf, note_size, 4, NT_GNU_BUILD_ID, "GNU", 4, &desc_size);

        if (desc) {
            size_t len = desc_size < max_len ? desc_size : max_len;
            memcpy(build_id, desc, len);
            build_id_len = len;
            sentry_free(note_buf);
            goto done;
        }

        sentry_free(note_buf);
    }

done:
    sentry_free(shdr_buf);
    close(fd);
    return build_id_len;
}

/**
 * A symbol-table source for a module: either the module's own ELF file or
 * its split-debug companion located via the `.gnu_debuglink` section.
 * Cached per module so the section-header parse and the debug-file search
 * run once per module rather than once per frame.
 */
typedef struct {
    char sym_path[SENTRY_CRASH_MAX_PATH];
    int state; // 0 = unused, 1 = usable, -1 = no symbol table found
    uint16_t e_type; // e_type of the loaded module (st_value semantics)
    uint64_t symtab_off;
    uint64_t symtab_size;
    uint64_t strtab_off;
    uint64_t strtab_size;
} sym_source_t;

// Upper bound on a resolved symbol name; mirrors dbghelp's MAX_SYM_NAME.
#    define SENTRY_MAX_SYM_NAME 2000

// Direct-mapped by module index; see get_sym_source. Being BSS, only the
// slots touched during resolution are committed to physical memory.
static sym_source_t g_sym_sources[SENTRY_CRASH_MAX_MODULES];

/**
 * Parse the ELF file at `path` and record the location of its symbol table
 * in `src`. Prefers the full `.symtab` and only falls back to `.dynsym`
 * when `allow_dynsym` is set: the dynamic table covers exported symbols
 * only, so lookups against it are unreliable for binaries whose `.symtab`
 * was stripped. When `debuglink_out` is non-NULL, also extracts the file
 * name stored in a `.gnu_debuglink` section, if present.
 * Returns 1 when a usable symbol table was found, 0 otherwise.
 */
static int
elf_locate_symtab(const char *path, int allow_dynsym, sym_source_t *src,
    char *debuglink_out, size_t debuglink_out_size, uint16_t *e_type_out)
{
    if (debuglink_out && debuglink_out_size > 0) {
        debuglink_out[0] = '\0';
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    Elf64_Ehdr ehdr;
#    else
    Elf32_Ehdr ehdr;
#    endif
    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)
        || memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0
        || !sentry__elf_is_native_class(ehdr.e_ident)
        || !sentry__elf_has_shdr_size(ehdr.e_ident, ehdr.e_shentsize)) {
        close(fd);
        return 0;
    }

    if (e_type_out) {
        *e_type_out = ehdr.e_type;
    }

    size_t shdr_size = (size_t)ehdr.e_shentsize * ehdr.e_shnum;
    void *shdr_buf = sentry_malloc(shdr_size);
    if (!shdr_buf) {
        close(fd);
        return 0;
    }

    if (lseek(fd, ehdr.e_shoff, SEEK_SET) != (off_t)ehdr.e_shoff
        || read(fd, shdr_buf, shdr_size) != (ssize_t)shdr_size) {
        sentry_free(shdr_buf);
        close(fd);
        return 0;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    Elf64_Shdr *sections = (Elf64_Shdr *)shdr_buf;
#    else
    Elf32_Shdr *sections = (Elf32_Shdr *)shdr_buf;
#    endif

    int symtab_idx = -1;
    int dynsym_idx = -1;
    for (int j = 0; j < ehdr.e_shnum; j++) {
        if (sections[j].sh_type == SHT_SYMTAB && symtab_idx < 0) {
            symtab_idx = j;
        } else if (sections[j].sh_type == SHT_DYNSYM && dynsym_idx < 0) {
            dynsym_idx = j;
        }
    }

    // Extract the .gnu_debuglink file name, identified via the section name
    // string table (the section has no dedicated sh_type).
    if (debuglink_out && ehdr.e_shstrndx < ehdr.e_shnum) {
        size_t shstr_size = sections[ehdr.e_shstrndx].sh_size;
        if (shstr_size > 0 && shstr_size <= 1024 * 1024) {
            char *shstr_buf = sentry_malloc(shstr_size);
            if (shstr_buf
                && lseek(fd, sections[ehdr.e_shstrndx].sh_offset, SEEK_SET)
                    == (off_t)sections[ehdr.e_shstrndx].sh_offset
                && read(fd, shstr_buf, shstr_size) == (ssize_t)shstr_size) {
                for (int j = 0; j < ehdr.e_shnum; j++) {
                    size_t name_off = sections[j].sh_name;
                    if (name_off >= shstr_size
                        || strnlen(shstr_buf + name_off, shstr_size - name_off)
                            == shstr_size - name_off
                        || strcmp(shstr_buf + name_off, ".gnu_debuglink")
                            != 0) {
                        continue;
                    }
                    // Content: NUL-terminated file name, padding, CRC32.
                    size_t link_size = sections[j].sh_size;
                    if (link_size < 5 || link_size > 4096 + 4) {
                        break;
                    }
                    char link_buf[4096 + 4];
                    if (lseek(fd, sections[j].sh_offset, SEEK_SET)
                            == (off_t)sections[j].sh_offset
                        && read(fd, link_buf, link_size)
                            == (ssize_t)link_size) {
                        size_t name_max = link_size - 4;
                        size_t name_len = strnlen(link_buf, name_max);
                        if (name_len > 0 && name_len < name_max
                            && name_len < debuglink_out_size) {
                            memcpy(debuglink_out, link_buf, name_len + 1);
                        }
                    }
                    break;
                }
            }
            sentry_free(shstr_buf);
        }
    }

    int chosen
        = symtab_idx >= 0 ? symtab_idx : (allow_dynsym ? dynsym_idx : -1);
    int found = 0;
    if (chosen >= 0
        && sentry__elf_has_sym_entsize(
            ehdr.e_ident, sections[chosen].sh_entsize)) {
        uint32_t strtab_idx = sections[chosen].sh_link;
        if (strtab_idx > 0 && strtab_idx < ehdr.e_shnum
            && sections[strtab_idx].sh_size > 0
            && (size_t)snprintf(
                   src->sym_path, sizeof(src->sym_path), "%s", path)
                < sizeof(src->sym_path)) {
            src->symtab_off = sections[chosen].sh_offset;
            src->symtab_size = sections[chosen].sh_size;
            src->strtab_off = sections[strtab_idx].sh_offset;
            src->strtab_size = sections[strtab_idx].sh_size;
            found = 1;
        }
    }

    sentry_free(shdr_buf);
    close(fd);
    return found;
}

/**
 * Resolve the symbol-table source for a module. If the module's own ELF was
 * stripped of its `.symtab`, follow the GDB split-debug conventions to find
 * the debug companion (`<dir>/<debuglink>`, `<dir>/.debug/<debuglink>`,
 * `/usr/lib/debug/<dir>/<debuglink>`), validated by GNU build-id equality.
 * Falls back to the module's `.dynsym` when no companion is found.
 * Returns NULL when the module has no usable symbol table.
 */
static const sym_source_t *
get_sym_source(const sentry_module_info_t *mod, uint32_t mod_idx)
{
    // The cache is direct-mapped by module index: within a frozen crash
    // context each index maps to exactly one module, so a slot is resolved at
    // most once and reused for every frame in that module. This covers all
    // modules the context can hold, so no frame is left unsymbolicated for
    // want of a cache slot.
    sym_source_t *slot = &g_sym_sources[mod_idx];
    if (slot->state != 0) {
        return slot->state > 0 ? slot : NULL;
    }
    slot->state = -1;

    // 1) The module's own .symtab (also picks up the .gnu_debuglink name).
    char debuglink[4096];
    uint16_t e_type = ET_DYN;
    if (elf_locate_symtab(
            mod->name, 0, slot, debuglink, sizeof(debuglink), &e_type)) {
        slot->e_type = e_type;
        slot->state = 1;
        return slot;
    }

    // 2) The split-debug companion. Candidates are validated by comparing
    // GNU build-ids; when either file lacks one, the candidate is accepted
    // as-is (the CRC32 stored in .gnu_debuglink is not verified here).
    if (debuglink[0]) {
        uint8_t mod_bid[16];
        size_t mod_bid_len = extract_elf_build_id_for_module(
            mod->name, mod_bid, sizeof(mod_bid));

        const char *slash = strrchr(mod->name, '/');
        int dir_len = slash ? (int)(slash - mod->name) : 0;

        char candidate[SENTRY_CRASH_MAX_PATH];
        for (int c = 0; slash && c < 3; c++) {
            int n;
            switch (c) {
            case 0:
                n = snprintf(candidate, sizeof(candidate), "%.*s/%s", dir_len,
                    mod->name, debuglink);
                break;
            case 1:
                n = snprintf(candidate, sizeof(candidate), "%.*s/.debug/%s",
                    dir_len, mod->name, debuglink);
                break;
            default:
                n = snprintf(candidate, sizeof(candidate),
                    "/usr/lib/debug%.*s/%s", dir_len, mod->name, debuglink);
                break;
            }
            if (n < 0 || (size_t)n >= sizeof(candidate)) {
                continue;
            }

            // The companion must carry a real .symtab; its .dynsym (if any)
            // adds nothing over the module's own.
            if (!elf_locate_symtab(candidate, 0, slot, NULL, 0, NULL)) {
                continue;
            }

            uint8_t dbg_bid[16];
            size_t dbg_bid_len = extract_elf_build_id_for_module(
                candidate, dbg_bid, sizeof(dbg_bid));
            if (mod_bid_len && dbg_bid_len
                && (mod_bid_len != dbg_bid_len
                    || memcmp(mod_bid, dbg_bid, mod_bid_len) != 0)) {
                SENTRY_DEBUGF(
                    "Split-debug candidate %s rejected: build-id mismatch",
                    candidate);
                continue;
            }

            SENTRY_DEBUGF("Using split-debug file %s for module %s", candidate,
                mod->name);
            slot->e_type = e_type;
            slot->state = 1;
            return slot;
        }
    }

    // 3) Fall back to the module's .dynsym.
    if (elf_locate_symtab(mod->name, 1, slot, NULL, 0, &e_type)) {
        slot->e_type = e_type;
        slot->state = 1;
        return slot;
    }

    return NULL;
}

/**
 * Find the symbol covering `sym_target` in the source's symbol table and
 * copy its name into `name_out`. The table is scanned in fixed-size chunks
 * and only the winning name is read from the string table, so large tables
 * (debug companions of big binaries) are never loaded wholesale. A symbol
 * only matches when the target lies within [st_value, st_value + st_size):
 * a nearest-preceding fallback without the size bound would attribute
 * addresses in symbol-table gaps to unrelated neighboring functions.
 * Returns 1 when a name was resolved.
 */
static int
sym_source_lookup(const sym_source_t *src, uint64_t sym_target, char *name_out,
    size_t name_out_size)
{
    int fd = open(src->sym_path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    Elf64_Sym chunk[256];
#    else
    Elf32_Sym chunk[256];
#    endif

    uint64_t best_value = 0;
    uint64_t best_name_off = 0;
    int have_best = 0;

    uint64_t sym_count = src->symtab_size / sizeof(chunk[0]);
    for (uint64_t idx = 1; idx < sym_count;) {
        size_t n = sym_count - idx;
        if (n > sizeof(chunk) / sizeof(chunk[0])) {
            n = sizeof(chunk) / sizeof(chunk[0]);
        }
        ssize_t want = (ssize_t)(n * sizeof(chunk[0]));
        if (pread(fd, chunk, (size_t)want,
                (off_t)(src->symtab_off + idx * sizeof(chunk[0])))
            != want) {
            break;
        }

        for (size_t k = 0; k < n; k++) {
#    if defined(__x86_64__) || defined(__aarch64__)
            unsigned char sym_type = ELF64_ST_TYPE(chunk[k].st_info);
#    else
            unsigned char sym_type = ELF32_ST_TYPE(chunk[k].st_info);
#    endif
            if (sym_type != STT_FUNC && sym_type != STT_NOTYPE) {
                continue;
            }
            if (chunk[k].st_value == 0 || sym_target < chunk[k].st_value
                || sym_target - chunk[k].st_value >= chunk[k].st_size) {
                continue;
            }
            if ((!have_best || chunk[k].st_value > best_value)
                && chunk[k].st_name != 0
                && chunk[k].st_name < src->strtab_size) {
                best_value = chunk[k].st_value;
                best_name_off = chunk[k].st_name;
                have_best = 1;
            }
        }

        idx += n;
    }

    int resolved = 0;
    if (have_best) {
        size_t max_read = src->strtab_size - best_name_off;
        if (max_read > name_out_size - 1) {
            max_read = name_out_size - 1;
        }
        ssize_t got = pread(
            fd, name_out, max_read, (off_t)(src->strtab_off + best_name_off));
        if (got > 0) {
            name_out[got] = '\0';
            resolved = name_out[0] != '\0';
        }
    }

    close(fd);
    return resolved;
}

/**
 * Resolve the symbol name for a given instruction address from the ELF
 * symbol table of the containing module (or its split-debug companion) and
 * set the "function" key on the frame value.
 */
static void
enrich_frame_with_symbol(
    const sentry_crash_context_t *ctx, sentry_value_t frame, uint64_t addr)
{
    uint32_t module_count = MIN(ctx->module_count, SENTRY_CRASH_MAX_MODULES);
    for (uint32_t i = 0; i < module_count; i++) {
        const sentry_module_info_t *mod = &ctx->modules[i];
        if (addr < mod->base_address || addr >= mod->base_address + mod->size) {
            continue;
        }

        const sym_source_t *src = get_sym_source(mod, i);
        if (!src) {
            return;
        }

        // For ET_DYN (shared libs, PIE) st_value is base-relative; for
        // ET_EXEC (non-PIE) st_value is an absolute virtual address.
        uint64_t sym_target
            = src->e_type == ET_EXEC ? addr : addr - mod->base_address;

        char name[SENTRY_MAX_SYM_NAME];
        if (sym_source_lookup(src, sym_target, name, sizeof(name))) {
            sentry_value_set_by_key(
                frame, "function", sentry_value_new_string(name));
        }
        return;
    }
}

/**
 * Capture modules from /proc/<pid>/maps for debug_meta
 * This is called from the daemon to populate ctx->modules[] on Linux,
 * since the signal handler cannot safely enumerate modules.
 */
static void
capture_modules_from_proc_maps(sentry_crash_context_t *ctx)
{
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", ctx->crashed_pid);

    FILE *f = fopen(maps_path, "r");
    if (!f) {
        SENTRY_WARNF("Failed to open %s for module capture", maps_path);
        return;
    }

    char line[1024];
    ctx->module_count = 0;

    while (fgets(line, sizeof(line), f)
        && ctx->module_count < SENTRY_CRASH_MAX_MODULES) {

        // Parse line: "start-end perms offset dev inode pathname"
        unsigned long long start, end, offset;
        char perms[5];
        int pathname_offset = 0;

        int parsed = sscanf(line, "%llx-%llx %4s %llx %*s %*s %n", &start, &end,
            perms, &offset, &pathname_offset);

        if (parsed < 4) {
            continue;
        }

        // Must have a valid pathname (not [stack], [heap], etc.)
        if (pathname_offset <= 0 || line[pathname_offset] == '\0'
            || line[pathname_offset] == '[' || line[pathname_offset] == '\n') {
            continue; // No pathname or special mapping like [stack]
        }

        const char *pathname = line + pathname_offset;
        // Trim newline
        size_t len = strlen(pathname);
        if (len > 0 && pathname[len - 1] == '\n') {
            len--;
        }
        if (len == 0) {
            continue;
        }

        // Check if this file is already captured - if so, extend size if needed
        sentry_module_info_t *existing_mod = NULL;
        for (uint32_t j = 0; j < ctx->module_count; j++) {
            if (strncmp(ctx->modules[j].name, pathname, len) == 0
                && ctx->modules[j].name[len] == '\0') {
                existing_mod = &ctx->modules[j];
                break;
            }
        }

        if (existing_mod) {
            // Update size to cover this mapping as well
            // The module spans from base_address to max(end) of all its
            // mappings
            uint64_t new_end = end;
            uint64_t current_end
                = existing_mod->base_address + existing_mod->size;
            if (new_end > current_end) {
                existing_mod->size = new_end - existing_mod->base_address;
            }
            continue;
        }

        sentry_module_info_t *mod = &ctx->modules[ctx->module_count];

        // Calculate base address: for PIE binaries, the file offset tells us
        // how far into the file this mapping starts, so we subtract it to get
        // the actual load base address
        mod->base_address = start - offset;
        // Initial size covers from base to end of this mapping
        mod->size = end - mod->base_address;

        // Copy pathname
        size_t copy_len
            = len < sizeof(mod->name) - 1 ? len : sizeof(mod->name) - 1;
        memcpy(mod->name, pathname, copy_len);
        mod->name[copy_len] = '\0';

        // Extract Build ID from ELF file
        memset(mod->uuid, 0, sizeof(mod->uuid));
        mod->pdb_age = 0; // Not used on Linux, only for Windows PE modules
        extract_elf_build_id_for_module(
            mod->name, mod->uuid, sizeof(mod->uuid));

        // Convert to little-endian GUID format for Sentry debug_id
        sentry__uuid_swap_guid_bytes(mod->uuid);

        SENTRY_DEBUGF("Captured module: %s base=0x%llx size=0x%llx", mod->name,
            (unsigned long long)mod->base_address,
            (unsigned long long)mod->size);

        ctx->module_count++;
    }

    fclose(f);
    SENTRY_DEBUGF("Captured %u modules from /proc/%d/maps", ctx->module_count,
        ctx->crashed_pid);
}

/**
 * Enumerate threads from /proc/<pid>/task for the native event
 * This is called from the daemon to populate ctx->platform.threads[] on Linux,
 * since the signal handler can only capture the crashing thread.
 */
static void
enumerate_threads_from_proc(sentry_crash_context_t *ctx)
{
    char task_path[64];
    snprintf(task_path, sizeof(task_path), "/proc/%d/task", ctx->crashed_pid);

    DIR *dir = opendir(task_path);
    if (!dir) {
        SENTRY_WARNF("Failed to open %s for thread enumeration", task_path);
        return;
    }

    // Keep the crashed thread at index 0 (already captured by signal handler)
    pid_t crashed_tid = ctx->platform.threads[0].tid;
    size_t thread_count = 1; // Start at 1 since we already have crashed thread

    // Read thread name for the crashed thread (index 0)
    ctx->platform.threads[0].name[0] = '\0';
    {
        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/task/%d/comm",
            ctx->crashed_pid, crashed_tid);
        FILE *comm_file = fopen(comm_path, "r");
        if (comm_file) {
            if (fgets(ctx->platform.threads[0].name,
                    sizeof(ctx->platform.threads[0].name), comm_file)) {
                // Trim trailing newline
                size_t len = strlen(ctx->platform.threads[0].name);
                if (len > 0 && ctx->platform.threads[0].name[len - 1] == '\n') {
                    ctx->platform.threads[0].name[len - 1] = '\0';
                }
            }
            fclose(comm_file);
        }
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) && thread_count < SENTRY_CRASH_MAX_THREADS) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        pid_t tid = (pid_t)atoi(entry->d_name);
        if (tid <= 0 || tid == crashed_tid) {
            continue; // Skip invalid or already-captured crashed thread
        }

        // Add this thread (without full context - just the TID)
        ctx->platform.threads[thread_count].tid = tid;
        memset(&ctx->platform.threads[thread_count].context, 0,
            sizeof(ctx->platform.threads[thread_count].context));

        // Read thread name from /proc/[pid]/task/[tid]/comm
        ctx->platform.threads[thread_count].name[0] = '\0';
        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/task/%d/comm",
            ctx->crashed_pid, tid);
        FILE *comm_file = fopen(comm_path, "r");
        if (comm_file) {
            if (fgets(ctx->platform.threads[thread_count].name,
                    sizeof(ctx->platform.threads[thread_count].name),
                    comm_file)) {
                // Trim trailing newline
                size_t len = strlen(ctx->platform.threads[thread_count].name);
                if (len > 0
                    && ctx->platform.threads[thread_count].name[len - 1]
                        == '\n') {
                    ctx->platform.threads[thread_count].name[len - 1] = '\0';
                }
            }
            fclose(comm_file);
        }

        thread_count++;
    }

    closedir(dir);

    ctx->platform.num_threads = thread_count;
    SENTRY_DEBUGF("Enumerated %zu threads from /proc/%d/task", thread_count,
        ctx->crashed_pid);
}
#endif // SENTRY_PLATFORM_LINUX || SENTRY_PLATFORM_ANDROID

#if defined(SENTRY_PLATFORM_WINDOWS)
#    include <psapi.h>
#    include <tlhelp32.h>

// Global handle for ReadProcessMemory callback (set during stack walk)
static HANDLE g_stack_walk_process = NULL;

/**
 * Custom read memory callback for StackWalk64 to read from crashed process
 */
static BOOL CALLBACK
stack_walk_read_memory(HANDLE hProcess, DWORD64 lpBaseAddress, PVOID lpBuffer,
    DWORD nSize, LPDWORD lpNumberOfBytesRead)
{
    (void)hProcess; // Use our global handle instead
    SIZE_T bytesRead = 0;
    BOOL result = ReadProcessMemory(g_stack_walk_process,
        (LPCVOID)(uintptr_t)lpBaseAddress, lpBuffer, nSize, &bytesRead);
    if (lpNumberOfBytesRead) {
        *lpNumberOfBytesRead = (DWORD)bytesRead;
    }
    return result;
}

/**
 * Walk stack using StackWalk64 for out-of-process unwinding
 * Returns number of frames captured
 */
static size_t
walk_stack_with_dbghelp(HANDLE hProcess, DWORD crashed_tid,
    const CONTEXT *ctx_record, const sentry_crash_context_t *crash_ctx,
    sentry_frame_info_t *frames, size_t max_frames)
{
    // Open thread handle for the crashed thread
    HANDLE hThread = OpenThread(
        THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, crashed_tid);
    if (!hThread) {
        SENTRY_WARNF("Failed to open thread %lu for stack walk",
            (unsigned long)crashed_tid);
        return 0;
    }

    // Set up global handle for read callback
    g_stack_walk_process = hProcess;

    // Build a dbghelp search path from every loaded module's directory so it
    // can find each module's PDB next to its DLL and resolve symbols
    static wchar_t *s_sym_search_path = NULL;
    static BOOL s_sym_search_path_built = FALSE;
    if (!s_sym_search_path_built && crash_ctx && crash_ctx->module_count > 0) {
        s_sym_search_path_built = TRUE;
        uint32_t module_count
            = MIN(crash_ctx->module_count, SENTRY_CRASH_MAX_MODULES);
        sentry_stringbuilder_t sb;
        sentry__stringbuilder_init(&sb);
        size_t appended = 0;
        for (uint32_t mi = 0; mi < module_count; mi++) {
            const char *path = crash_ctx->modules[mi].name;
            size_t dir_len = 0;
            for (size_t k = 0; path[k]; k++) {
                if (path[k] == '\\' || path[k] == '/') {
                    dir_len = k;
                }
            }
            if (dir_len == 0) {
                continue;
            }
            // de-duplicate the directory against earlier modules
            BOOL dup = FALSE;
            for (uint32_t pj = 0; pj < mi && !dup; pj++) {
                const char *pp = crash_ctx->modules[pj].name;
                size_t pj_len = 0;
                for (size_t k = 0; pp[k]; k++) {
                    if (pp[k] == '\\' || pp[k] == '/') {
                        pj_len = k;
                    }
                }
                dup = (pj_len == dir_len && _strnicmp(pp, path, dir_len) == 0);
            }
            if (dup) {
                continue;
            }
            if (appended++) {
                sentry__stringbuilder_append_char(&sb, ';');
            }
            sentry__stringbuilder_append_buf(&sb, path, dir_len);
        }
        char *path_utf8 = sentry__stringbuilder_into_string(&sb);
        if (path_utf8 && path_utf8[0]) {
            s_sym_search_path = sentry__string_to_wstr(path_utf8);
        }
        sentry_free(path_utf8);
    }

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS
        | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitializeW(hProcess, s_sym_search_path, TRUE)) {
        SENTRY_WARNF("SymInitialize failed: %lu", GetLastError());
        CloseHandle(hThread);
        return 0;
    }

    CONTEXT ctx = *ctx_record;
    STACKFRAME64 stack_frame;
    memset(&stack_frame, 0, sizeof(stack_frame));

    DWORD machine_type;
#    if defined(_M_AMD64)
    machine_type = IMAGE_FILE_MACHINE_AMD64;
    stack_frame.AddrPC.Offset = ctx.Rip;
    stack_frame.AddrFrame.Offset = ctx.Rbp;
    stack_frame.AddrStack.Offset = ctx.Rsp;
#    elif defined(_M_IX86)
    machine_type = IMAGE_FILE_MACHINE_I386;
    stack_frame.AddrPC.Offset = ctx.Eip;
    stack_frame.AddrFrame.Offset = ctx.Ebp;
    stack_frame.AddrStack.Offset = ctx.Esp;
#    elif defined(_M_ARM64)
    machine_type = IMAGE_FILE_MACHINE_ARM64;
    stack_frame.AddrPC.Offset = ctx.Pc;
#        if defined(NONAMELESSUNION)
    stack_frame.AddrFrame.Offset = ctx.DUMMYUNIONNAME.DUMMYSTRUCTNAME.Fp;
#        else
    stack_frame.AddrFrame.Offset = ctx.Fp;
#        endif
    stack_frame.AddrStack.Offset = ctx.Sp;
#    elif defined(_M_ARM)
    machine_type = IMAGE_FILE_MACHINE_ARM;
    stack_frame.AddrPC.Offset = ctx.Pc;
    stack_frame.AddrFrame.Offset = ctx.R11;
    stack_frame.AddrStack.Offset = ctx.Sp;
#    else
    // Unsupported architecture
    SymCleanup(hProcess);
    CloseHandle(hThread);
    return 0;
#    endif
    stack_frame.AddrPC.Mode = AddrModeFlat;
    stack_frame.AddrFrame.Mode = AddrModeFlat;
    stack_frame.AddrStack.Mode = AddrModeFlat;

    const size_t sym_info_size
        = sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR);
    SYMBOL_INFOW *sym_info = (SYMBOL_INFOW *)sentry_malloc(sym_info_size);

    size_t frame_count = 0;
    while (frame_count < max_frames
        && StackWalk64(machine_type, hProcess, hThread, &stack_frame, &ctx,
            stack_walk_read_memory, SymFunctionTableAccess64,
            SymGetModuleBase64, NULL)) {
        if (stack_frame.AddrPC.Offset == 0) {
            break;
        }
        memset(&frames[frame_count], 0, sizeof(sentry_frame_info_t));
        frames[frame_count].instruction_addr
            = (void *)(uintptr_t)stack_frame.AddrPC.Offset;

        if (sym_info) {
            memset(sym_info, 0, sym_info_size);
            sym_info->SizeOfStruct = sizeof(SYMBOL_INFOW);
            sym_info->MaxNameLen = MAX_SYM_NAME;

            if (SymFromAddrW(
                    hProcess, stack_frame.AddrPC.Offset, NULL, sym_info)) {
                frames[frame_count].symbol
                    = sentry__string_from_wstr(sym_info->Name);
                frames[frame_count].symbol_addr
                    = (void *)(uintptr_t)sym_info->Address;
            }
        }

        SENTRY_DEBUGF("StackWalk64 frame %zu: 0x%llx %s", frame_count,
            (unsigned long long)stack_frame.AddrPC.Offset,
            frames[frame_count].symbol ? frames[frame_count].symbol : "");
        frame_count++;
    }

    sentry_free(sym_info);

    SymCleanup(hProcess);
    CloseHandle(hThread);
    g_stack_walk_process = NULL;

    SENTRY_DEBUGF("StackWalk64 captured %zu frames", frame_count);
    return frame_count;
}

/**
 * Extract PE TimeDateStamp from a module file for code_id
 * Returns 0 on failure
 */
static DWORD
get_pe_timestamp(const char *module_path)
{
    HANDLE hFile = CreateFileA(module_path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return 0;
    }

    // Read DOS header
    IMAGE_DOS_HEADER dos_header;
    DWORD bytes_read;
    if (!ReadFile(hFile, &dos_header, sizeof(dos_header), &bytes_read, NULL)
        || bytes_read != sizeof(dos_header) || dos_header.e_magic != 0x5A4D) {
        CloseHandle(hFile);
        return 0;
    }

    // Seek to PE header
    if (SetFilePointer(hFile, dos_header.e_lfanew, NULL, FILE_BEGIN)
        == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        return 0;
    }

    // Read PE signature and COFF header
    DWORD pe_sig;
    IMAGE_FILE_HEADER coff_header;
    if (!ReadFile(hFile, &pe_sig, sizeof(pe_sig), &bytes_read, NULL)
        || bytes_read != sizeof(pe_sig) || pe_sig != 0x00004550) {
        CloseHandle(hFile);
        return 0;
    }
    if (!ReadFile(hFile, &coff_header, sizeof(coff_header), &bytes_read, NULL)
        || bytes_read != sizeof(coff_header)) {
        CloseHandle(hFile);
        return 0;
    }

    CloseHandle(hFile);
    return coff_header.TimeDateStamp;
}

// CodeView signature for PDB 7.0 format (RSDS)
#    define CV_SIGNATURE_RSDS 0x53445352

/**
 * CodeView PDB 7.0 debug info structure
 */
struct CodeViewRecord70 {
    uint32_t signature;
    GUID pdb_signature;
    uint32_t pdb_age;
    char pdb_filename[1];
};

/**
 * Extract PDB debug info (GUID, age, and filename) from a module in another
 * process. Uses ReadProcessMemory to read PE headers from the crashed process.
 *
 * @param hProcess Handle to the crashed process
 * @param module_base Base address of the module in the crashed process
 * @param uuid Output: 16-byte PDB GUID (set to zeros on failure)
 * @param pdb_age Output: PDB age value (set to 0 on failure)
 * @param pdb_name Output: PDB filename buffer (set to empty on failure)
 * @param pdb_name_size Size of pdb_name buffer
 * @return true if PDB info was successfully extracted
 */
static bool
extract_pdb_info_from_process(HANDLE hProcess, uint64_t module_base,
    uint8_t *uuid, uint32_t *pdb_age, char *pdb_name, size_t pdb_name_size)
{
    // Initialize outputs to zero/empty
    memset(uuid, 0, 16);
    *pdb_age = 0;
    if (pdb_name && pdb_name_size > 0) {
        pdb_name[0] = '\0';
    }

    if (!module_base) {
        return false;
    }

    // Read DOS header
    IMAGE_DOS_HEADER dos_header;
    SIZE_T bytes_read;
    if (!ReadProcessMemory(hProcess, (LPCVOID)(uintptr_t)module_base,
            &dos_header, sizeof(dos_header), &bytes_read)
        || bytes_read != sizeof(dos_header)
        || dos_header.e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    // Read NT headers
    IMAGE_NT_HEADERS nt_headers;
    uint64_t nt_headers_addr = module_base + (uint64_t)dos_header.e_lfanew;
    if (!ReadProcessMemory(hProcess, (LPCVOID)(uintptr_t)nt_headers_addr,
            &nt_headers, sizeof(nt_headers), &bytes_read)
        || bytes_read != sizeof(nt_headers)
        || nt_headers.Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    // Get debug directory
    IMAGE_DATA_DIRECTORY debug_dir
        = nt_headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];

    if (debug_dir.VirtualAddress == 0 || debug_dir.Size == 0) {
        // No debug directory
        return false;
    }

    // Iterate through debug directory entries
    size_t entry_count = debug_dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (size_t i = 0; i < entry_count; i++) {
        IMAGE_DEBUG_DIRECTORY debug_entry;
        uint64_t entry_addr = module_base + debug_dir.VirtualAddress
            + i * sizeof(IMAGE_DEBUG_DIRECTORY);

        if (!ReadProcessMemory(hProcess, (LPCVOID)(uintptr_t)entry_addr,
                &debug_entry, sizeof(debug_entry), &bytes_read)
            || bytes_read != sizeof(debug_entry)) {
            continue;
        }

        // Look for CodeView debug info
        if (debug_entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
            continue;
        }

        // Read CodeView header (just the fixed part, not the variable filename)
        // The structure is: signature (4) + GUID (16) + age (4) + filename[]
        uint8_t cv_header[24]; // signature + GUID + age
        uint64_t cv_addr = module_base + debug_entry.AddressOfRawData;

        if (!ReadProcessMemory(hProcess, (LPCVOID)(uintptr_t)cv_addr, cv_header,
                sizeof(cv_header), &bytes_read)
            || bytes_read != sizeof(cv_header)) {
            continue;
        }

        // Check for RSDS signature (PDB 7.0)
        uint32_t cv_sig;
        memcpy(&cv_sig, cv_header, sizeof(cv_sig));
        if (cv_sig != CV_SIGNATURE_RSDS) {
            continue;
        }

        // Extract GUID (bytes 4-19)
        memcpy(uuid, cv_header + 4, 16);

        // Extract age (bytes 20-23)
        memcpy(pdb_age, cv_header + 20, sizeof(*pdb_age));

        // Extract PDB filename (variable length, null-terminated, starts at
        // byte 24) The filename can be up to (SizeOfData - 24) bytes
        if (pdb_name && pdb_name_size > 0) {
            size_t max_filename_len = pdb_name_size - 1;
            if (debug_entry.SizeOfData > 24) {
                size_t available = debug_entry.SizeOfData - 24;
                if (available < max_filename_len) {
                    max_filename_len = available;
                }
            }
            uint64_t filename_addr = cv_addr + 24;
            SIZE_T filename_read;
            if (ReadProcessMemory(hProcess, (LPCVOID)(uintptr_t)filename_addr,
                    pdb_name, max_filename_len, &filename_read)) {
                pdb_name[filename_read] = '\0';
                // Ensure null termination within buffer
                pdb_name[pdb_name_size - 1] = '\0';
            }
        }

        SENTRY_DEBUGF("Extracted PDB info: age=%u, pdb=%s", *pdb_age,
            pdb_name ? pdb_name : "(null)");
        return true;
    }

    return false;
}

/**
 * Capture modules from the crashed process for debug_meta on Windows
 */
static void
capture_modules_from_process(sentry_crash_context_t *ctx)
{
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, ctx->crashed_pid);
    if (!hProcess) {
        SENTRY_WARNF("Failed to open process %d for module enumeration",
            ctx->crashed_pid);
        return;
    }

    HMODULE hMods[SENTRY_CRASH_MAX_MODULES];
    DWORD cbNeeded;

    if (!EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        SENTRY_WARN("EnumProcessModules failed");
        CloseHandle(hProcess);
        return;
    }

    DWORD module_count = cbNeeded / sizeof(HMODULE);
    if (module_count > SENTRY_CRASH_MAX_MODULES) {
        module_count = SENTRY_CRASH_MAX_MODULES;
    }

    ctx->module_count = 0;
    for (DWORD i = 0; i < module_count; i++) {
        sentry_module_info_t *mod = &ctx->modules[ctx->module_count];

        // Get module file name
        char modName[MAX_PATH];
        if (!GetModuleFileNameExA(
                hProcess, hMods[i], modName, sizeof(modName))) {
            continue;
        }

        // Get module info for base address and size
        MODULEINFO modInfo;
        if (!GetModuleInformation(
                hProcess, hMods[i], &modInfo, sizeof(modInfo))) {
            continue;
        }

        mod->base_address = (uint64_t)(uintptr_t)modInfo.lpBaseOfDll;
        mod->size = (uint64_t)modInfo.SizeOfImage;
        strncpy(mod->name, modName, sizeof(mod->name) - 1);
        mod->name[sizeof(mod->name) - 1] = '\0';

        // Extract PDB GUID, age, and filename from PE debug directory
        extract_pdb_info_from_process(hProcess, mod->base_address, mod->uuid,
            &mod->pdb_age, mod->pdb_name, sizeof(mod->pdb_name));

        SENTRY_DEBUGF("Captured module: %s base=0x%llx size=0x%llx pdb_age=%u",
            mod->name, (unsigned long long)mod->base_address,
            (unsigned long long)mod->size, mod->pdb_age);

        ctx->module_count++;
    }

    CloseHandle(hProcess);
    SENTRY_DEBUGF("Captured %u modules from process %d", ctx->module_count,
        ctx->crashed_pid);
}

/**
 * Check if thread ID already exists in the threads array
 */
static bool
thread_id_exists(
    const sentry_crash_context_t *ctx, DWORD thread_id, DWORD count)
{
    for (DWORD i = 0; i < count; i++) {
        if (ctx->platform.threads[i].thread_id == thread_id) {
            return true;
        }
    }
    return false;
}

/**
 * Retrieve the thread name via GetThreadDescription (Windows 10 1607+).
 */
static void
get_thread_name(HANDLE hThread, sentry_thread_context_windows_t *tctx)
{
    typedef HRESULT(WINAPI * GetThreadDescription_t)(HANDLE, PWSTR *);
    static GetThreadDescription_t get_thread_description
        = (GetThreadDescription_t)-1;
    if (get_thread_description == (GetThreadDescription_t)-1) {
        get_thread_description = (GetThreadDescription_t)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    }

    tctx->name[0] = '\0';
    if (!get_thread_description || !hThread) {
        return;
    }

    PWSTR description = NULL;
    HRESULT hr = get_thread_description(hThread, &description);
    if (SUCCEEDED(hr) && description && description[0] != L'\0') {
        WideCharToMultiByte(CP_UTF8, 0, description, -1, tctx->name,
            sizeof(tctx->name), NULL, NULL);
        tctx->name[sizeof(tctx->name) - 1] = '\0';
    }
    LocalFree(description);
}

/**
 * Enumerate threads from the crashed process for the native event on Windows
 * Captures thread contexts for stack walking.
 *
 * Not available on Xbox (the ToolHelp32 API is not part of the gaming
 * partition). MiniDumpWriteDump still captures all thread contexts from the
 * target process independently, so skipping this only loses supplementary
 * per-thread metadata from the Sentry event payload.
 */
static void
enumerate_threads_from_process(sentry_crash_context_t *ctx)
{
#    if defined(SENTRY_PLATFORM_XBOX)
    (void)ctx;
    return;
#    else
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        SENTRY_WARN("CreateToolhelp32Snapshot failed");
        return;
    }

    // Keep the crashed thread at index 0 (already captured)
    DWORD crashed_tid = (DWORD)ctx->crashed_tid;
    DWORD thread_count = 1;

    // Retrieve the name for the crashed thread (index 0)
    HANDLE hCrashedThread
        = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, crashed_tid);
    get_thread_name(hCrashedThread, &ctx->platform.threads[0]);
    if (hCrashedThread) {
        CloseHandle(hCrashedThread);
    }

    SENTRY_DEBUGF(
        "enumerate_threads: start, crashed_tid=%lu, threads[0].id=%lu",
        (unsigned long)crashed_tid,
        (unsigned long)ctx->platform.threads[0].thread_id);

    THREADENTRY32 te32;
    te32.dwSize = sizeof(THREADENTRY32);

    if (Thread32First(hSnapshot, &te32)) {
        do {
            // Skip if not our process, is the crashed thread, or already seen
            if (te32.th32OwnerProcessID != (DWORD)ctx->crashed_pid
                || te32.th32ThreadID == crashed_tid
                || thread_count >= SENTRY_CRASH_MAX_THREADS) {
                continue;
            }

            // Check for duplicates (defensive - shouldn't happen normally)
            if (thread_id_exists(ctx, te32.th32ThreadID, thread_count)) {
                SENTRY_WARNF("Duplicate thread ID %lu in snapshot, skipping",
                    (unsigned long)te32.th32ThreadID);
                continue;
            }

            // Open thread to capture its context
            HANDLE hThread = OpenThread(THREAD_GET_CONTEXT
                    | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                FALSE, te32.th32ThreadID);

            if (hThread == NULL) {
                SENTRY_WARNF("Failed to open thread %lu: %lu",
                    (unsigned long)te32.th32ThreadID, GetLastError());
                continue;
            }

            // Retrieve thread name before suspending
            get_thread_name(hThread, &ctx->platform.threads[thread_count]);

            // Suspend thread to safely capture context
            // (likely already suspended due to crash, but be safe)
            DWORD suspend_count = SuspendThread(hThread);
            bool was_running = (suspend_count == 0);

            // Capture thread context
            CONTEXT thread_ctx;
            memset(&thread_ctx, 0, sizeof(thread_ctx));
            thread_ctx.ContextFlags = CONTEXT_FULL;

            BOOL got_context = GetThreadContext(hThread, &thread_ctx);

            // Resume thread if we suspended it
            if (was_running || suspend_count != (DWORD)-1) {
                ResumeThread(hThread);
            }

            CloseHandle(hThread);

            if (!got_context) {
                SENTRY_WARNF("Failed to get context for thread %lu: %lu",
                    (unsigned long)te32.th32ThreadID, GetLastError());
                continue;
            }

            // Store thread info with captured context
            ctx->platform.threads[thread_count].thread_id = te32.th32ThreadID;
            ctx->platform.threads[thread_count].context = thread_ctx;
            thread_count++;

            SENTRY_DEBUGF("Captured context for thread %lu",
                (unsigned long)te32.th32ThreadID);
        } while (Thread32Next(hSnapshot, &te32));
    }

    CloseHandle(hSnapshot);

    ctx->platform.num_threads = thread_count;
    SENTRY_DEBUGF("Enumerated %u threads from process %d", thread_count,
        ctx->crashed_pid);
#    endif // SENTRY_PLATFORM_XBOX
}
#endif // SENTRY_PLATFORM_WINDOWS

/**
 * Build stacktrace for the crashed thread.
 * Convenience wrapper around build_stacktrace_for_thread().
 */
static sentry_value_t
build_stacktrace_from_ctx(const sentry_crash_context_t *ctx)
{
    return build_stacktrace_for_thread(ctx, SIZE_MAX);
}

/**
 * Reads one breadcrumb ring file the crashing process appended on its hot path
 * into a breadcrumb list. Returns null if the file is absent or empty.
 */
static sentry_value_t
read_breadcrumb_ring_file(const sentry_path_t *run_folder, const char *name)
{
    if (!run_folder) {
        return sentry_value_new_null();
    }
    sentry_path_t *path = sentry__path_join_str(run_folder, name);
    if (!path) {
        return sentry_value_new_null();
    }
    size_t size = 0;
    char *buf = sentry__path_read_to_buffer(path, &size);
    sentry__path_free(path);
    if (!buf || size == 0) {
        sentry_free(buf);
        return sentry_value_new_null();
    }
    sentry_value_t list = sentry__value_from_msgpack(buf, size);
    sentry_free(buf);
    // `sentry__value_from_msgpack` only builds a list when the file holds 2+
    // concatenated values; a file with a single breadcrumb decodes to a bare
    // object. Wrap it so the merge step (which ignores non-lists) keeps it.
    if (sentry_value_get_type(list) == SENTRY_VALUE_TYPE_OBJECT) {
        sentry_value_t wrapper = sentry_value_new_list();
        sentry_value_append(wrapper, list);
        return wrapper;
    }
    return list;
}

/**
 * Assembles the crash event's breadcrumbs from the two ring files the crashing
 * process appended one-at-a-time, merges them in timestamp order, keeps the
 * newest `max_breadcrumbs`, and attaches them to `event`.
 * Mirrors the crashpad backend's `report_to_envelope`.
 */
static void
apply_breadcrumbs_from_ring_files(sentry_value_t event,
    const sentry_path_t *run_folder, const sentry_crash_context_t *ctx)
{
    if (ctx && ctx->max_breadcrumbs == 0) {
        return;
    }

    sentry_value_t b1
        = read_breadcrumb_ring_file(run_folder, "__sentry-breadcrumb1");
    sentry_value_t b2
        = read_breadcrumb_ring_file(run_folder, "__sentry-breadcrumb2");
    size_t max = ctx && ctx->max_breadcrumbs ? ctx->max_breadcrumbs
                                             : SENTRY_BREADCRUMBS_MAX;
    sentry_value_t merged = sentry__value_merge_breadcrumbs(b1, b2, max);
    sentry_value_decref(b1);
    sentry_value_decref(b2);
    // Overwrite any breadcrumbs the base event may carry: the ring files are
    // the single source of truth, so this is idempotent and never duplicates.
    if (sentry_value_get_type(merged) == SENTRY_VALUE_TYPE_LIST) {
        sentry_value_set_by_key(event, "breadcrumbs", merged);
    } else {
        sentry_value_decref(merged);
    }
}

/**
 * Build a native event and set the level, mechanism, and handled state
 *
 * @param ctx Crash context
 * @param event_file_path Path to base event file from parent process
 * @param run_folder Run directory holding the breadcrumb ring files
 * @param level Event level (e.g. "fatal")
 * @param mechanism_type Exception mechanism type (e.g. "signalhandler")
 * @param handled Whether the mechanism was handled
 */
static sentry_value_t
build_native_event(const sentry_crash_context_t *ctx,
    const char *event_file_path, const sentry_path_t *run_folder,
    const char *level, const char *mechanism_type, bool handled)
{
    // Read base event from parent's file
    sentry_value_t event = sentry_value_new_null();
    if (event_file_path && event_file_path[0]) {
        sentry_path_t *ev_path = sentry__path_from_str(event_file_path);
        if (ev_path) {
            size_t event_size = 0;
            char *event_json
                = sentry__path_read_to_buffer(ev_path, &event_size);
            sentry__path_free(ev_path);
            if (event_json && event_size > 0) {
                event = sentry__value_from_json(event_json, event_size);
                sentry_free(event_json);
            }
        }
    }

    if (sentry_value_is_null(event)) {
        event = sentry_value_new_event();
    } else {
        sentry__ensure_event_id(event, NULL);
    }

    apply_breadcrumbs_from_ring_files(event, run_folder, ctx);

    // Set platform to native
    sentry_value_set_by_key(
        event, "platform", sentry_value_new_string("native"));

    sentry_value_set_by_key(event, "level", sentry_value_new_string(level));

    // Build exception
    const char *signal_name = "UNKNOWN";
#if defined(SENTRY_PLATFORM_UNIX)
    int signal_number = ctx->platform.signum;
    signal_name = get_signal_name(signal_number);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // Exception code is used directly below as unsigned
    signal_name = "EXCEPTION";
#endif

    sentry_value_t exc = sentry_value_new_object();
    sentry_value_set_by_key(exc, "type", sentry_value_new_string(signal_name));

    char value_buf[128];
    snprintf(value_buf, sizeof(value_buf), "Fatal crash: %s", signal_name);
    sentry_value_set_by_key(exc, "value", sentry_value_new_string(value_buf));

    // Add mechanism
    sentry_value_t mechanism = sentry_value_new_object();
    sentry_value_set_by_key(
        mechanism, "type", sentry_value_new_string(mechanism_type));
    sentry_value_set_by_key(
        mechanism, "synthetic", sentry_value_new_bool(true));
    sentry_value_set_by_key(
        mechanism, "handled", sentry_value_new_bool(handled));

    // Add signal metadata
    sentry_value_t meta = sentry_value_new_object();
    sentry_value_t signal_info = sentry_value_new_object();
#if defined(SENTRY_PLATFORM_WINDOWS)
    // Windows exception codes are unsigned 32-bit values (e.g., 0xC0000005)
    // Use uint64 to preserve the unsigned value for the symbolicator
    sentry_value_set_by_key(signal_info, "number",
        sentry_value_new_uint64((uint64_t)ctx->platform.exception_code));
#else
    sentry_value_set_by_key(
        signal_info, "number", sentry_value_new_int32(signal_number));
#endif
    sentry_value_set_by_key(
        signal_info, "name", sentry_value_new_string(signal_name));
    sentry_value_set_by_key(meta, "signal", signal_info);
    sentry_value_set_by_key(mechanism, "meta", meta);

    sentry_value_set_by_key(exc, "mechanism", mechanism);

    // Add stacktrace to exception
    sentry_value_set_by_key(exc, "stacktrace", build_stacktrace_from_ctx(ctx));

    // Wrap exception in values array
    sentry_value_t exceptions = sentry_value_new_object();
    sentry_value_t exc_values = sentry_value_new_list();
    sentry_value_append(exc_values, exc);
    sentry_value_set_by_key(exceptions, "values", exc_values);
    sentry_value_set_by_key(event, "exception", exceptions);

    // Always add threads with names to the event JSON
    {
        sentry_value_t threads = sentry_value_new_object();
        sentry_value_t thread_values = sentry_value_new_list();

#if defined(SENTRY_PLATFORM_MACOS)
        // Add all captured threads
        for (size_t i = 0; i < ctx->platform.num_threads; i++) {
            const sentry_thread_context_darwin_t *tctx
                = &ctx->platform.threads[i];
            sentry_value_t thread = sentry_value_new_object();

            sentry_value_set_by_key(
                thread, "id", sentry_value_new_int32((int32_t)tctx->tid));

            // Set thread name if available
            if (tctx->name[0] != '\0') {
                sentry_value_set_by_key(
                    thread, "name", sentry_value_new_string(tctx->name));
            }

            bool is_crashed = (tctx->tid == (uint64_t)ctx->crashed_tid);
            sentry_value_set_by_key(
                thread, "crashed", sentry_value_new_bool(is_crashed));
            sentry_value_set_by_key(
                thread, "current", sentry_value_new_bool(is_crashed));

            // Build stacktrace for non-crashed threads only
            // (crashed thread's stacktrace is already in exception.values)
            if (!is_crashed) {
                sentry_value_t stacktrace = build_stacktrace_for_thread(ctx, i);
                if (!sentry_value_is_null(stacktrace)) {
                    sentry_value_set_by_key(thread, "stacktrace", stacktrace);
                }
            }

            sentry_value_append(thread_values, thread);
        }
        SENTRY_DEBUGF("Added %zu threads to event", ctx->platform.num_threads);
#elif defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
        // Add all captured threads
        for (size_t i = 0; i < ctx->platform.num_threads; i++) {
            const sentry_thread_context_linux_t *tctx
                = &ctx->platform.threads[i];
            sentry_value_t thread = sentry_value_new_object();

            sentry_value_set_by_key(
                thread, "id", sentry_value_new_int32((int32_t)tctx->tid));

            // Set thread name if available
            if (tctx->name[0] != '\0') {
                sentry_value_set_by_key(
                    thread, "name", sentry_value_new_string(tctx->name));
            }

            bool is_crashed = (tctx->tid == ctx->crashed_tid);
            sentry_value_set_by_key(
                thread, "crashed", sentry_value_new_bool(is_crashed));
            sentry_value_set_by_key(
                thread, "current", sentry_value_new_bool(is_crashed));

            // Build stacktrace for non-crashed threads only
            // (crashed thread's stacktrace is already in exception.values)
            if (!is_crashed) {
                sentry_value_t stacktrace = build_stacktrace_for_thread(ctx, i);
                if (!sentry_value_is_null(stacktrace)) {
                    sentry_value_set_by_key(thread, "stacktrace", stacktrace);
                }
            }

            sentry_value_append(thread_values, thread);
        }
        SENTRY_DEBUGF("Added %zu threads to event", ctx->platform.num_threads);
#elif defined(SENTRY_PLATFORM_WINDOWS)
        // Add all captured threads
        SENTRY_DEBUGF("Windows: adding %lu threads to event JSON",
            (unsigned long)ctx->platform.num_threads);
        for (DWORD i = 0; i < ctx->platform.num_threads; i++) {
            const sentry_thread_context_windows_t *tctx
                = &ctx->platform.threads[i];
            sentry_value_t thread = sentry_value_new_object();

            sentry_value_set_by_key(
                thread, "id", sentry_value_new_int32((int32_t)tctx->thread_id));

            // Set thread name if available
            if (tctx->name[0] != '\0') {
                sentry_value_set_by_key(
                    thread, "name", sentry_value_new_string(tctx->name));
            }

            bool is_crashed = (tctx->thread_id == (DWORD)ctx->crashed_tid);
            sentry_value_set_by_key(
                thread, "crashed", sentry_value_new_bool(is_crashed));
            sentry_value_set_by_key(
                thread, "current", sentry_value_new_bool(is_crashed));

            // Build stacktrace for non-crashed threads only
            // (crashed thread's stacktrace is already in exception.values)
            if (!is_crashed) {
                sentry_value_t stacktrace = build_stacktrace_for_thread(ctx, i);
                if (!sentry_value_is_null(stacktrace)) {
                    sentry_value_set_by_key(thread, "stacktrace", stacktrace);
                }
            }

            sentry_value_append(thread_values, thread);
        }
        SENTRY_DEBUGF("Added %lu threads to event",
            (unsigned long)ctx->platform.num_threads);
#else
        // Fallback: just add the crashed thread (without stacktrace since
        // it's already in exception.values)
        sentry_value_t crashed_thread = sentry_value_new_object();
        sentry_value_set_by_key(crashed_thread, "id",
            sentry_value_new_int32((int32_t)ctx->crashed_tid));
        sentry_value_set_by_key(
            crashed_thread, "crashed", sentry_value_new_bool(true));
        sentry_value_set_by_key(
            crashed_thread, "current", sentry_value_new_bool(true));
        // Note: stacktrace is NOT added here - it's in exception.values[0]
        sentry_value_append(thread_values, crashed_thread);
#endif

        sentry_value_set_by_key(threads, "values", thread_values);
        sentry_value_set_by_key(event, "threads", threads);
    }

    // Add debug_meta with module images from crashed process
    // (ctx->modules[] was captured in the signal handler of the crashed
    // process)
    uint32_t module_count = MIN(ctx->module_count, SENTRY_CRASH_MAX_MODULES);
    SENTRY_DEBUGF("Module count for debug_meta: %u", module_count);
    if (module_count > 0) {
        sentry_value_t images = sentry_value_new_list();

        for (uint32_t i = 0; i < module_count; i++) {
            const sentry_module_info_t *mod = &ctx->modules[i];
            sentry_value_t image = sentry_value_new_object();

            // Set image type based on platform
#if defined(SENTRY_PLATFORM_MACOS)
            sentry_value_set_by_key(
                image, "type", sentry_value_new_string("macho"));
#elif defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
            sentry_value_set_by_key(
                image, "type", sentry_value_new_string("elf"));
#elif defined(SENTRY_PLATFORM_WINDOWS)
            sentry_value_set_by_key(
                image, "type", sentry_value_new_string("pe"));

            // Set arch for Windows PE modules (required for Sentry
            // symbolication)
#    if defined(_M_AMD64)
            sentry_value_set_by_key(
                image, "arch", sentry_value_new_string("x86_64"));
#    elif defined(_M_IX86)
            sentry_value_set_by_key(
                image, "arch", sentry_value_new_string("x86"));
#    elif defined(_M_ARM64)
            sentry_value_set_by_key(
                image, "arch", sentry_value_new_string("arm64"));
#    endif
#endif

            // Set code_file (path to the module)
            if (mod->name[0]) {
                sentry_value_set_by_key(
                    image, "code_file", sentry_value_new_string(mod->name));
            }

            // Set image_addr as hex string
            char addr_buf[32];
            snprintf(
                addr_buf, sizeof(addr_buf), "0x%" PRIx64, mod->base_address);
            sentry_value_set_by_key(
                image, "image_addr", sentry_value_new_string(addr_buf));

            sentry_value_set_by_key(image, "image_size",
                sentry_value_new_int64((int64_t)mod->size));

#if defined(SENTRY_PLATFORM_WINDOWS)
            // Set code_id for PE modules (TimeDateStamp + SizeOfImage)
            // Format: 8-digit zero-padded timestamp (uppercase) + size
            // (uppercase) Must match sentry_modulefinder_windows.c format
            if (mod->name[0]) {
                DWORD timestamp = get_pe_timestamp(mod->name);
                if (timestamp != 0) {
                    char code_id_buf[32];
                    snprintf(code_id_buf, sizeof(code_id_buf), "%08lX%lX",
                        (unsigned long)timestamp, (unsigned long)mod->size);
                    // Ensure uppercase (defensive - some runtimes may differ)
                    for (char *p = code_id_buf; *p; p++) {
                        if (*p >= 'a' && *p <= 'f') {
                            *p = *p - 'a' + 'A';
                        }
                    }
                    sentry_value_set_by_key(
                        image, "code_id", sentry_value_new_string(code_id_buf));
                }
            }

            // Set debug_id from PDB GUID + age (format: GUID-age)
            // Only set if we have valid PDB info (non-zero GUID)
            // The GUID bytes from PE are in Windows mixed-endian format
            // (Data1/2/3 are little-endian, Data4 is big-endian)
            {
                // Check if UUID is non-zero (valid PDB info was extracted)
                bool has_valid_uuid = false;
                for (int j = 0; j < 16; j++) {
                    if (mod->uuid[j] != 0) {
                        has_valid_uuid = true;
                        break;
                    }
                }

                if (has_valid_uuid) {
                    // Copy to aligned GUID structure to avoid alignment issues
                    // (mod->uuid is uint8_t[] with 1-byte alignment, GUID
                    // needs 4)
                    GUID guid;
                    memcpy(&guid, mod->uuid, sizeof(GUID));
                    sentry_uuid_t uuid = sentry__uuid_from_native(&guid);
                    char debug_id_buf[50]; // GUID (36) + '-' (1) + age (up to
                                           // 10) + null
                    sentry_uuid_as_string(&uuid, debug_id_buf);
                    debug_id_buf[36] = '-';
                    snprintf(debug_id_buf + 37, 12, "%x",
                        (unsigned int)mod->pdb_age);
                    sentry_value_set_by_key(image, "debug_id",
                        sentry_value_new_string(debug_id_buf));
                }
            }

            // Set debug_file (path to PDB file for symbolication)
            if (mod->pdb_name[0]) {
                sentry_value_set_by_key(image, "debug_file",
                    sentry_value_new_string(mod->pdb_name));
            }
#else
            // Set debug_id from UUID (macOS/Linux)
            sentry_uuid_t uuid
                = sentry_uuid_from_bytes((const char *)mod->uuid);
            sentry_value_set_by_key(
                image, "debug_id", sentry__value_new_uuid(&uuid));
#endif

            sentry_value_append(images, image);
        }

        sentry_value_t debug_meta = sentry_value_new_object();
        sentry_value_set_by_key(debug_meta, "images", images);
        sentry_value_set_by_key(event, "debug_meta", debug_meta);
        SENTRY_DEBUGF("Added %u modules from crashed process to debug_meta",
            module_count);
    } else {
        SENTRY_WARN("No modules captured - debug_meta.images will be empty!");
    }

    return event;
}

/**
 * Write envelope with native stacktrace event
 * If minidump_path is provided, also attach it as an attachment
 */
static bool
write_envelope_with_native_stacktrace(const sentry_options_t *options,
    const char *envelope_path, const sentry_crash_context_t *ctx,
    const char *event_file_path, const char *minidump_path,
    sentry_path_t *run_folder)
{
    // Build native crash event (always include threads with names)
    SENTRY_DEBUGF("write_envelope_with_native_stacktrace: minidump_path=%s",
        minidump_path ? minidump_path : "(null)");
    sentry_value_t event = build_native_event(
        ctx, event_file_path, run_folder, "fatal", "signalhandler", false);

    // Serialize event to JSON
    size_t event_size = 0;
    char *event_json = sentry__value_to_json(event, &event_size);
    char *event_id = sentry__string_clone(
        sentry_value_as_string(sentry_value_get_by_key(event, "event_id")));
    sentry_value_decref(event);

    if (!event_json) {
        SENTRY_WARN("Failed to serialize native crash event to JSON");
        sentry_free(event_id);
        return false;
    }

    // Open envelope file for writing
#if defined(SENTRY_PLATFORM_UNIX)
    int fd = open(envelope_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    wchar_t *wpath = sentry__string_to_wstr(envelope_path);
    int fd = wpath ? _wopen(wpath, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                         _S_IREAD | _S_IWRITE)
                   : -1;
    sentry_free(wpath);
#endif
    if (fd < 0) {
        SENTRY_WARN("Failed to open envelope file for writing");
        sentry_free(event_json);
        sentry_free(event_id);
        return false;
    }

    // Write envelope header
    const char *dsn
        = options && options->dsn ? sentry_options_get_dsn(options) : NULL;
    char header_buf[SENTRY_CRASH_ENVELOPE_HEADER_SIZE];
    int header_len;
    if (dsn && !sentry__string_empty(event_id)) {
        header_len = snprintf(header_buf, sizeof(header_buf),
            "{\"dsn\":\"%s\",\"event_id\":\"%s\"}\n", dsn, event_id);
    } else if (dsn) {
        header_len = snprintf(
            header_buf, sizeof(header_buf), "{\"dsn\":\"%s\"}\n", dsn);
    } else if (!sentry__string_empty(event_id)) {
        header_len = snprintf(header_buf, sizeof(header_buf),
            "{\"event_id\":\"%s\"}\n", event_id);
    } else {
        header_len = snprintf(header_buf, sizeof(header_buf), "{}\n");
    }
    sentry_free(event_id);
    if (header_len > 0 && header_len < (int)sizeof(header_buf)) {
#if defined(SENTRY_PLATFORM_UNIX)
        if (write(fd, header_buf, header_len) != header_len) {
            SENTRY_WARN("Failed to write envelope header");
        }
#elif defined(SENTRY_PLATFORM_WINDOWS)
        _write(fd, header_buf, (unsigned int)header_len);
#endif
    }

    // Write event item
    char event_header[SENTRY_CRASH_ITEM_HEADER_SIZE];
    int ev_header_len = snprintf(event_header, sizeof(event_header),
        "{\"type\":\"event\",\"length\":%zu}\n", event_size);
    if (ev_header_len > 0 && ev_header_len < (int)sizeof(event_header)) {
#if defined(SENTRY_PLATFORM_UNIX)
        if (write(fd, event_header, ev_header_len) != ev_header_len) {
            SENTRY_WARN("Failed to write event header to envelope");
        }
        if (write(fd, event_json, event_size) != (ssize_t)event_size) {
            SENTRY_WARN("Failed to write event data to envelope");
        }
        if (write(fd, "\n", 1) != 1) {
            SENTRY_WARN("Failed to write event newline to envelope");
        }
#elif defined(SENTRY_PLATFORM_WINDOWS)
        _write(fd, event_header, (unsigned int)ev_header_len);
        _write(fd, event_json, (unsigned int)event_size);
        _write(fd, "\n", 1);
#endif
    }

    sentry_free(event_json);

    // Add minidump as attachment if provided
    if (minidump_path && minidump_path[0]) {
#if defined(SENTRY_PLATFORM_UNIX)
        int minidump_fd = open(minidump_path, O_RDONLY);
#elif defined(SENTRY_PLATFORM_WINDOWS)
        wchar_t *wpath_md = sentry__string_to_wstr(minidump_path);
        int minidump_fd
            = wpath_md ? _wopen(wpath_md, _O_RDONLY | _O_BINARY) : -1;
        sentry_free(wpath_md);
#endif
        if (minidump_fd >= 0) {
#if defined(SENTRY_PLATFORM_UNIX)
            struct stat st;
            if (fstat(minidump_fd, &st) == 0) {
                long long minidump_size = (long long)st.st_size;
#elif defined(SENTRY_PLATFORM_WINDOWS)
            struct __stat64 st;
            if (_fstat64(minidump_fd, &st) == 0) {
                long long minidump_size = (long long)st.st_size;
#endif
                // Write minidump attachment header
                char md_header[SENTRY_CRASH_ITEM_HEADER_SIZE];
                int md_header_len = snprintf(md_header, sizeof(md_header),
                    "{\"type\":\"attachment\",\"length\":%lld,"
                    "\"attachment_type\":\"event.minidump\","
                    "\"filename\":\"minidump.dmp\"}\n",
                    minidump_size);

                if (md_header_len > 0
                    && md_header_len < (int)sizeof(md_header)) {
#if defined(SENTRY_PLATFORM_UNIX)
                    if (write(fd, md_header, md_header_len) != md_header_len) {
                        SENTRY_WARN(
                            "Failed to write minidump header to envelope");
                    }
#elif defined(SENTRY_PLATFORM_WINDOWS)
                    _write(fd, md_header, (unsigned int)md_header_len);
#endif
                }

                // Copy minidump content
                char buf[SENTRY_CRASH_FILE_BUFFER_SIZE];
#if defined(SENTRY_PLATFORM_UNIX)
                ssize_t n;
                while ((n = read(minidump_fd, buf, sizeof(buf))) > 0) {
                    if (write(fd, buf, n) != n) {
                        SENTRY_WARN("Failed to write minidump to envelope");
                        break;
                    }
                }
                if (write(fd, "\n", 1) != 1) {
                    SENTRY_WARN("Failed to write newline to envelope");
                }
                close(minidump_fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
                int n;
                while ((n = _read(minidump_fd, buf, sizeof(buf))) > 0) {
                    _write(fd, buf, (unsigned int)n);
                }
                _write(fd, "\n", 1);
                _close(minidump_fd);
#endif
            } else {
#if defined(SENTRY_PLATFORM_UNIX)
                close(minidump_fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
                _close(minidump_fd);
#endif
            }
        }
    }

    // Add scope attachments using metadata file
    if (run_folder) {
        sentry_path_t *attach_list_path
            = sentry__path_join_str(run_folder, "__sentry-attachments");
        if (attach_list_path) {
            size_t attach_json_len = 0;
            char *attach_json = sentry__path_read_to_buffer(
                attach_list_path, &attach_json_len);
            sentry__path_free(attach_list_path);

            if (attach_json && attach_json_len > 0) {
                // Parse attachment list JSON
                sentry_value_t attach_list
                    = sentry__value_from_json(attach_json, attach_json_len);
                sentry_free(attach_json);

                if (!sentry_value_is_null(attach_list)) {
                    size_t len = sentry_value_get_length(attach_list);
                    for (size_t i = 0; i < len; i++) {
                        sentry_value_t attach_info
                            = sentry_value_get_by_index(attach_list, i);
                        sentry_value_t path_val
                            = sentry_value_get_by_key(attach_info, "path");
                        sentry_value_t filename_val
                            = sentry_value_get_by_key(attach_info, "filename");
                        sentry_value_t attachment_type_val
                            = sentry_value_get_by_key(
                                attach_info, "attachment_type");
                        sentry_value_t content_type_val
                            = sentry_value_get_by_key(
                                attach_info, "content_type");

                        const char *path = sentry_value_as_string(path_val);
                        const char *filename
                            = sentry_value_as_string(filename_val);
                        const char *attachment_type
                            = sentry_value_as_string(attachment_type_val);
                        const char *content_type
                            = sentry_value_as_string(content_type_val);

                        if (path && filename
                            && !attachment_is_placeholder(options, path)) {
                            write_attachment_to_envelope(fd, path, filename,
                                attachment_type, content_type);
                        }
                    }
                    sentry_value_decref(attach_list);
                }
            }
        }
    }

    // Add screenshot attachment if captured by the daemon
    if (ctx->attach_screenshot && run_folder) {
        sentry_path_t *screenshot_path
            = sentry__path_join_str(run_folder, "screenshot.png");
        if (screenshot_path) {
            write_attachment_to_envelope(
                fd, screenshot_path->path, "screenshot.png", NULL, "image/png");
            sentry__path_free(screenshot_path);
        }
    }

    // Add session replay attachment if captured by the daemon
    if (ctx->attach_session_replay && run_folder) {
        sentry_path_t *replay_path
            = sentry__path_join_str(run_folder, "session-replay.mp4");
        if (replay_path) {
            write_attachment_to_envelope(
                fd, replay_path->path, "session-replay.mp4", NULL, "video/mp4");
            sentry__path_free(replay_path);
        }
    }

#if defined(SENTRY_PLATFORM_UNIX)
    close(fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    _close(fd);
#endif

    return true;
}

/**
 * Manually write a Sentry envelope with event, minidump, and attachments.
 * Format matches what Crashpad's Envelope class does.
 */
static bool
write_envelope_with_minidump(const sentry_options_t *options,
    const sentry_crash_context_t *ctx, const char *envelope_path,
    const char *event_msgpack_path, const char *minidump_path,
    sentry_path_t *run_folder)
{
    // Read the base event, merge in the breadcrumbs from the ring files,
    // re-serialize.
    size_t event_size = 0;
    char *event_json = NULL;
    char *event_id = NULL;
    sentry_path_t *ev_path = sentry__path_from_str(event_msgpack_path);
    if (ev_path) {
        size_t base_size = 0;
        char *base_json = sentry__path_read_to_buffer(ev_path, &base_size);
        sentry__path_free(ev_path);
        if (base_json && base_size > 0) {
            sentry_value_t event
                = sentry__value_from_json(base_json, base_size);
            if (sentry_value_is_null(event)) {
                // Parsing the base event failed (e.g. truncated buffer or
                // OOM). Don't serialize the null into "null" and ship an
                // invalid payload - fall back to streaming the raw event
                // bytes verbatim so the crash report is preserved.
                sentry_value_decref(event);
                event_json = sentry__string_clone_n(base_json, base_size);
                event_size = event_json ? base_size : 0;
            } else {
                apply_breadcrumbs_from_ring_files(event, run_folder, ctx);
                event_id = sentry__string_clone(sentry_value_as_string(
                    sentry_value_get_by_key(event, "event_id")));
                event_json = sentry__value_to_json(event, &event_size);
                sentry_value_decref(event);
                if (!event_json) {
                    // Re-serialization failed (e.g. OOM). Fall back to the raw
                    // event bytes so the crash report is preserved, losing only
                    // the merged breadcrumbs rather than the whole event.
                    event_json = sentry__string_clone_n(base_json, base_size);
                    event_size = event_json ? base_size : 0;
                }
            }
        }
        sentry_free(base_json);
    }

    // Open envelope file for writing
#if defined(SENTRY_PLATFORM_UNIX)
    int fd = open(envelope_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // Use wide-char API for proper UTF-8 path support
    wchar_t *wpath = sentry__string_to_wstr(envelope_path);
    int fd = wpath ? _wopen(wpath, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                         _S_IREAD | _S_IWRITE)
                   : -1;
    sentry_free(wpath);
#endif
    if (fd < 0) {
        SENTRY_WARN("Failed to open envelope file for writing");
        sentry_free(event_json);
        sentry_free(event_id);
        return false;
    }

    // Write envelope headers (just DSN and event ID if available)
    const char *dsn
        = options && options->dsn ? sentry_options_get_dsn(options) : NULL;
    char header_buf[SENTRY_CRASH_ENVELOPE_HEADER_SIZE];
    int header_len;
    if (dsn && !sentry__string_empty(event_id)) {
        header_len = snprintf(header_buf, sizeof(header_buf),
            "{\"dsn\":\"%s\",\"event_id\":\"%s\"}\n", dsn, event_id);
    } else if (dsn) {
        header_len = snprintf(
            header_buf, sizeof(header_buf), "{\"dsn\":\"%s\"}\n", dsn);
    } else if (!sentry__string_empty(event_id)) {
        header_len = snprintf(header_buf, sizeof(header_buf),
            "{\"event_id\":\"%s\"}\n", event_id);
    } else {
        header_len = snprintf(header_buf, sizeof(header_buf), "{}\n");
    }
    sentry_free(event_id);
    if (header_len > 0 && header_len < (int)sizeof(header_buf)) {
#if defined(SENTRY_PLATFORM_UNIX)
        if (write(fd, header_buf, header_len) != header_len) {
            SENTRY_WARN("Failed to write envelope header");
        }
#elif defined(SENTRY_PLATFORM_WINDOWS)
        _write(fd, header_buf, (unsigned int)header_len);
#endif
    }

    // Write event item
    if (event_json && event_size > 0) {
        char event_header[SENTRY_CRASH_ITEM_HEADER_SIZE];
        int ev_header_len = snprintf(event_header, sizeof(event_header),
            "{\"type\":\"event\",\"length\":%zu}\n", event_size);
        if (ev_header_len > 0 && ev_header_len < (int)sizeof(event_header)) {
#if defined(SENTRY_PLATFORM_UNIX)
            if (write(fd, event_header, ev_header_len) != ev_header_len) {
                SENTRY_WARN("Failed to write event header to envelope");
            }
            if (write(fd, event_json, event_size) != (ssize_t)event_size) {
                SENTRY_WARN("Failed to write event data to envelope");
            }
            if (write(fd, "\n", 1) != 1) {
                SENTRY_WARN("Failed to write event newline to envelope");
            }
#elif defined(SENTRY_PLATFORM_WINDOWS)
            _write(fd, event_header, (unsigned int)ev_header_len);
            _write(fd, event_json, (unsigned int)event_size);
            _write(fd, "\n", 1);
#endif
        }
    }
    sentry_free(event_json);

    // Add minidump as attachment
#if defined(SENTRY_PLATFORM_UNIX)
    int minidump_fd = open(minidump_path, O_RDONLY);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // Use wide-char API for proper UTF-8 path support
    wchar_t *wpath_md = sentry__string_to_wstr(minidump_path);
    int minidump_fd = wpath_md ? _wopen(wpath_md, _O_RDONLY | _O_BINARY) : -1;
    sentry_free(wpath_md);
#endif
    if (minidump_fd >= 0) {
#if defined(SENTRY_PLATFORM_UNIX)
        struct stat st;
        if (fstat(minidump_fd, &st) == 0) {
            long long minidump_size = (long long)st.st_size;
#elif defined(SENTRY_PLATFORM_WINDOWS)
        struct __stat64 st;
        if (_fstat64(minidump_fd, &st) == 0) {
            long long minidump_size = (long long)st.st_size;
#endif
            // Write minidump item header
            char minidump_header[SENTRY_CRASH_ITEM_HEADER_SIZE];
            int md_header_len
                = snprintf(minidump_header, sizeof(minidump_header),
                    "{\"type\":\"attachment\",\"length\":%lld,"
                    "\"attachment_type\":\"event.minidump\","
                    "\"filename\":\"minidump.dmp\"}\n",
                    minidump_size);

            if (md_header_len > 0
                && md_header_len < (int)sizeof(minidump_header)) {
#if defined(SENTRY_PLATFORM_UNIX)
                if (write(fd, minidump_header, md_header_len)
                    != md_header_len) {
                    SENTRY_WARN("Failed to write minidump header to envelope");
                }
#elif defined(SENTRY_PLATFORM_WINDOWS)
                _write(fd, minidump_header, (unsigned int)md_header_len);
#endif
            }

            // Copy minidump content
            char buf[SENTRY_CRASH_READ_BUFFER_SIZE];
#if defined(SENTRY_PLATFORM_UNIX)
            ssize_t n;
            while ((n = read(minidump_fd, buf, sizeof(buf))) > 0) {
                if (write(fd, buf, (size_t)n) != n) {
                    SENTRY_WARN("Failed to write minidump data to envelope");
                    break;
                }
            }
            if (n < 0) {
                SENTRY_WARN("Failed to read minidump data");
            }
            if (write(fd, "\n", 1) != 1) {
                SENTRY_WARN("Failed to write minidump newline to envelope");
            }
#elif defined(SENTRY_PLATFORM_WINDOWS)
            int n;
            while ((n = _read(minidump_fd, buf, sizeof(buf))) > 0) {
                _write(fd, buf, (unsigned int)n);
            }
            _write(fd, "\n", 1);
#endif
        }
#if defined(SENTRY_PLATFORM_UNIX)
        close(minidump_fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
        _close(minidump_fd);
#endif
    }

    // Add scope attachments using metadata file
    if (run_folder) {
        sentry_path_t *attach_list_path
            = sentry__path_join_str(run_folder, "__sentry-attachments");
        if (attach_list_path) {
            size_t attach_json_len = 0;
            char *attach_json = sentry__path_read_to_buffer(
                attach_list_path, &attach_json_len);
            sentry__path_free(attach_list_path);

            if (attach_json && attach_json_len > 0) {
                // Parse attachment list JSON
                sentry_value_t attach_list
                    = sentry__value_from_json(attach_json, attach_json_len);
                sentry_free(attach_json);

                if (!sentry_value_is_null(attach_list)) {
                    size_t len = sentry_value_get_length(attach_list);
                    for (size_t i = 0; i < len; i++) {
                        sentry_value_t attach_info
                            = sentry_value_get_by_index(attach_list, i);
                        sentry_value_t path_val
                            = sentry_value_get_by_key(attach_info, "path");
                        sentry_value_t filename_val
                            = sentry_value_get_by_key(attach_info, "filename");
                        sentry_value_t attachment_type_val
                            = sentry_value_get_by_key(
                                attach_info, "attachment_type");
                        sentry_value_t content_type_val
                            = sentry_value_get_by_key(
                                attach_info, "content_type");

                        const char *path = sentry_value_as_string(path_val);
                        const char *filename
                            = sentry_value_as_string(filename_val);
                        const char *attachment_type
                            = sentry_value_as_string(attachment_type_val);
                        const char *content_type
                            = sentry_value_as_string(content_type_val);

                        if (path && filename
                            && !attachment_is_placeholder(options, path)) {
                            write_attachment_to_envelope(fd, path, filename,
                                attachment_type, content_type);
                        }
                    }
                    sentry_value_decref(attach_list);
                }
            }
        }
    }

    // Add screenshot attachment if captured by the daemon
    if (ctx->attach_screenshot && run_folder) {
        sentry_path_t *screenshot_path
            = sentry__path_join_str(run_folder, "screenshot.png");
        if (screenshot_path) {
            write_attachment_to_envelope(
                fd, screenshot_path->path, "screenshot.png", NULL, "image/png");
            sentry__path_free(screenshot_path);
        }
    }

    // Add session replay attachment if captured by the daemon
    if (ctx->attach_session_replay && run_folder) {
        sentry_path_t *replay_path
            = sentry__path_join_str(run_folder, "session-replay.mp4");
        if (replay_path) {
            write_attachment_to_envelope(
                fd, replay_path->path, "session-replay.mp4", NULL, "video/mp4");
            sentry__path_free(replay_path);
        }
    }

#if defined(SENTRY_PLATFORM_UNIX)
    close(fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    _close(fd);
#endif
    SENTRY_DEBUG("Envelope written successfully");
    return true;
}

/**
 * Process crash and generate minidump
 * Uses Sentry's API to reuse all existing functionality
 *
 * Called by the crash daemon (out-of-process on Linux/macOS).
 */
bool
sentry__process_crash(const sentry_options_t *options, sentry_crash_ipc_t *ipc)
{
    SENTRY_DEBUG("Processing crash - START");
    bool crash_captured = false;

    sentry_crash_context_t *ctx = ipc->shmem;

    // Mark as processing
    sentry__atomic_store(&ctx->state, SENTRY_CRASH_STATE_PROCESSING);
    SENTRY_DEBUG("Marked state as PROCESSING");

    // Check crash reporting mode
    int mode = ctx->crash_reporting_mode;
    SENTRY_DEBUGF("Crash reporting mode: %d", mode);

    // Determine if we need to write a minidump
    // Mode 0 (MINIDUMP): Always write minidump
    // Mode 1 (NATIVE): No minidump
    // Mode 2 (NATIVE_WITH_MINIDUMP): Write minidump
    bool need_minidump = (mode == SENTRY_CRASH_REPORTING_MODE_MINIDUMP
        || mode == SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP);

    // Determine if we use native stacktrace mode
    // Mode 0: Use minidump-only envelope (existing behavior)
    // Mode 1 & 2: Use native stacktrace envelope
    bool use_native_mode = (mode == SENTRY_CRASH_REPORTING_MODE_NATIVE
        || mode == SENTRY_CRASH_REPORTING_MODE_NATIVE_WITH_MINIDUMP);

    // Generate minidump path in database directory
    char minidump_path[SENTRY_CRASH_MAX_PATH] = { 0 };
    const char *db_dir = ctx->database_path;

    if (need_minidump) {
        int path_len = snprintf(minidump_path, sizeof(minidump_path),
            "%s/sentry-minidump-%lu-%lu.dmp", db_dir,
            (unsigned long)ctx->crashed_pid, (unsigned long)ctx->crashed_tid);

        if (path_len < 0 || path_len >= (int)sizeof(minidump_path)) {
            SENTRY_WARN("Minidump path truncated or invalid");
            goto done;
        }

        SENTRY_DEBUGF("Writing minidump to: %s", minidump_path);
        SENTRY_DEBUGF(
            "About to call sentry__write_minidump, ctx=%p, crashed_pid=%d",
            (void *)ctx, ctx->crashed_pid);

        // Write minidump
        int minidump_result = sentry__write_minidump(ctx, minidump_path);
        SENTRY_DEBUGF("sentry__write_minidump returned: %d", minidump_result);

        if (minidump_result != 0) {
            SENTRY_WARN("Failed to write minidump");
            minidump_path[0] = '\0'; // Clear path on failure
        } else {
            SENTRY_DEBUG("Minidump written successfully");

            // Copy minidump path back to shared memory
#ifdef _WIN32
            strncpy_s(ctx->minidump_path, sizeof(ctx->minidump_path),
                minidump_path, _TRUNCATE);
#else
            size_t mp_len = strlen(minidump_path);
            size_t copy_len = mp_len < sizeof(ctx->minidump_path) - 1
                ? mp_len
                : sizeof(ctx->minidump_path) - 1;
            memcpy(ctx->minidump_path, minidump_path, copy_len);
            ctx->minidump_path[copy_len] = '\0';
#endif
        }
    }

    // For mode 0 (MINIDUMP only), we need a successful minidump
    if (mode == SENTRY_CRASH_REPORTING_MODE_MINIDUMP
        && minidump_path[0] == '\0') {
        SENTRY_WARN("Minidump mode requires minidump, but minidump failed");
        goto done;
    }

    // Get event file path from context
    const char *event_path = ctx->event_path[0] ? ctx->event_path : NULL;
    SENTRY_DEBUGF(
        "Event path from context: %s", event_path ? event_path : "(null)");
    if (!event_path) {
        SENTRY_WARN("No event file from parent");
        if (minidump_path[0]) {
            // Delete the orphaned minidump to prevent disk space leaks
#if defined(SENTRY_PLATFORM_UNIX)
            unlink(minidump_path);
#elif defined(SENTRY_PLATFORM_WINDOWS)
            wchar_t *wpath = sentry__string_to_wstr(minidump_path);
            if (wpath) {
                _wunlink(wpath);
                sentry_free(wpath);
            }
#endif
        }
        ctx->minidump_path[0] = '\0';
        goto done;
    }

    // Extract run folder path from event path (event is at
    // run_folder/__sentry-event)
    SENTRY_DEBUG("Extracting run folder from event path");
    sentry_path_t *ev_path = sentry__path_from_str(event_path);
    sentry_path_t *run_folder = ev_path ? sentry__path_dir(ev_path) : NULL;

    // Acquire the run directory lock file so that process_old_runs() in a
    // new SDK run will skip this directory while the daemon is still
    // processing the crash. The crashed process's flock() is released on
    // death, so without this the new run could delete the directory.
    sentry_filelock_t *run_lock = NULL;
    if (run_folder) {
        sentry_path_t *lock_path = sentry__path_append_str(run_folder, ".lock");
        if (lock_path) {
            run_lock = sentry__filelock_new(lock_path);
            if (run_lock) {
                if (!sentry__filelock_try_lock(run_lock)) {
                    SENTRY_WARN("daemon could not acquire run folder lock");
                    sentry__filelock_free(run_lock);
                    run_lock = NULL;
                }
            }
        }
    }

    // Create envelope file in database directory
    char envelope_path[SENTRY_CRASH_MAX_PATH];
    int path_len = snprintf(envelope_path, sizeof(envelope_path),
        "%s/sentry-envelope-%lu.env", db_dir, (unsigned long)ctx->crashed_pid);

    if (path_len < 0 || path_len >= (int)sizeof(envelope_path)) {
        SENTRY_WARN("Envelope path truncated or invalid");
        sentry__path_free(ev_path);
        if (run_folder) {
            sentry__path_free(run_folder);
        }
        if (run_lock) {
            sentry__filelock_unlock(run_lock);
            sentry__filelock_free(run_lock);
        }
        goto done;
    }

    SENTRY_DEBUGF("Creating envelope at: %s", envelope_path);

    // Capture screenshot if enabled (Windows only)
    // This is done in the daemon process (out-of-process) because
    // screenshot capture is NOT signal-safe (uses LoadLibrary, GDI+, etc.)
#if !defined(SENTRY_SCREENSHOT_NONE)
    if (ctx->attach_screenshot && run_folder) {
        SENTRY_DEBUG("Capturing screenshot");
        sentry_path_t *screenshot_path
            = sentry__path_join_str(run_folder, "screenshot.png");
        if (screenshot_path) {
            // Pass the crashed app's PID so we capture its windows, not the
            // daemon's
            if (sentry__screenshot_capture(
                    screenshot_path, (uint32_t)ctx->crashed_pid)) {
                SENTRY_DEBUG("Screenshot captured successfully");
            } else {
                SENTRY_DEBUG("Screenshot capture failed");
            }
            sentry__path_free(screenshot_path);
        }
    }

    // Capture session replay if enabled. Like screenshot, this runs
    // out-of-process because the underlying OS APIs are not signal-safe.
    if (ctx->attach_session_replay && run_folder) {
        SENTRY_DEBUG("Capturing session replay");
        sentry_path_t *replay_path
            = sentry__path_join_str(run_folder, "session-replay.mp4");
        if (replay_path) {
            if (sentry__session_replay_capture(replay_path,
                    ctx->session_replay_duration, (uint32_t)ctx->crashed_pid)) {
                SENTRY_DEBUG("Session replay captured successfully");
            } else {
                SENTRY_DEBUG("Session replay capture failed");
            }
            sentry__path_free(replay_path);
        }
    }
#endif

    // On Linux, capture modules and threads from /proc for native mode
    // This must be done before the crashed process exits
#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    if (use_native_mode) {
        if (ctx->module_count == 0) {
            SENTRY_DEBUG("Capturing modules from /proc/maps for debug_meta");
            capture_modules_from_proc_maps(ctx);
        }
        if (ctx->platform.num_threads <= 1) {
            SENTRY_DEBUG("Enumerating threads from /proc/task");
            enumerate_threads_from_proc(ctx);
        }
    }
#endif

    // On Windows, capture modules and threads from the crashed process
#if defined(SENTRY_PLATFORM_WINDOWS)
    if (use_native_mode) {
        if (ctx->module_count == 0) {
            SENTRY_DEBUG("Capturing modules from crashed process");
            capture_modules_from_process(ctx);
        }
        if (ctx->platform.num_threads <= 1) {
            SENTRY_DEBUG("Enumerating threads from crashed process");
            enumerate_threads_from_process(ctx);
        }
    }
#endif

    // Write envelope based on mode
    bool envelope_written = false;
    SENTRY_DEBUGF("Envelope decision: mode=%d, use_native_mode=%d, "
                  "need_minidump=%d, minidump_path='%s'",
        mode, use_native_mode, need_minidump,
        minidump_path[0] ? minidump_path : "(empty)");
    if (use_native_mode) {
        // Mode 1 (NATIVE) or Mode 2 (NATIVE_WITH_MINIDUMP)
        SENTRY_DEBUGF("Writing envelope with native stacktrace, passing "
                      "minidump_path=%s",
            minidump_path[0] ? minidump_path : "NULL");
        envelope_written = write_envelope_with_native_stacktrace(options,
            envelope_path, ctx, event_path,
            minidump_path[0] ? minidump_path : NULL, run_folder);
    } else {
        // Mode 0 (MINIDUMP only)
        SENTRY_DEBUG("Writing envelope with minidump");
        envelope_written = write_envelope_with_minidump(
            options, ctx, envelope_path, event_path, minidump_path, run_folder);
    }

    if (!envelope_written) {
        SENTRY_WARN("Failed to write envelope");
        sentry__path_free(ev_path);
        if (run_folder) {
            sentry__path_free(run_folder);
        }
        if (run_lock) {
            sentry__filelock_unlock(run_lock);
            sentry__filelock_free(run_lock);
        }
        goto done;
    }
    SENTRY_DEBUG("Envelope written successfully");

    // Read envelope and send via transport
    SENTRY_DEBUG("Reading envelope file back");

    // Check if file exists and get size
#if defined(SENTRY_PLATFORM_WINDOWS)
    wchar_t *wenvelope_path = sentry__string_to_wstr(envelope_path);
    struct _stat64 st;
    if (wenvelope_path && _wstat64(wenvelope_path, &st) == 0) {
        SENTRY_DEBUGF(
            "Envelope file exists, size=%lld bytes", (long long)st.st_size);
    } else {
        SENTRY_WARNF("Envelope file stat failed: %s", strerror(errno));
    }
    sentry_free(wenvelope_path);
#else
    struct stat st;
    if (stat(envelope_path, &st) == 0) {
        SENTRY_DEBUGF("Envelope file exists, size=%ld bytes", (long)st.st_size);
    } else {
        SENTRY_WARNF("Envelope file stat failed: %s", strerror(errno));
    }
#endif

    if (options->run) {
        sentry__atomic_store(&options->run->user_consent,
            sentry__atomic_fetch(&ctx->user_consent));
    }

    sentry_path_t *env_path = sentry__path_from_str(envelope_path);
    if (!env_path) {
        SENTRY_WARN("Failed to create envelope path");
        goto cleanup;
    }

    sentry_envelope_t *envelope = sentry__envelope_from_path(env_path);
    sentry__path_free(env_path);

    if (!envelope) {
        SENTRY_WARN("Failed to read envelope file");
        goto cleanup;
    }

    add_attachment_refs(envelope, options, run_folder);

    bool has_attachment_refs = sentry__envelope_has_content_type(
        envelope, SENTRY_ATTACHMENT_REF_MIME);

    SENTRY_DEBUG("Envelope loaded, capturing");

    if (options->cache_keep && options->external_crash_reporter
        && !sentry__envelope_materialize(envelope)) {
        SENTRY_WARN("Failed to materialize envelope for external crash report");
    }

    // Capture directly, or pass to external crash reporter
    if (!sentry__launch_external_crash_reporter(options, envelope)) {
        if (has_attachment_refs && options && options->run) {
            if (!sentry__run_write_envelope(options->run, envelope)) {
                SENTRY_WARN(
                    "Failed to dump crash envelope for resend on restart");
            }
        }
        if (options && options->transport && options->run) {
            SENTRY_DEBUG("Capturing crash envelope");
            sentry__capture_envelope(options->transport, envelope, options);
            crash_captured = true;
            SENTRY_DEBUG("Crash envelope captured (queued)");
        } else {
            SENTRY_WARN("No transport available for sending envelope");
            sentry_envelope_free(envelope);
        }
    } else {
        crash_captured = true;
    }

    // Clean up temporary envelope file (keep minidump for
    // inspection/debugging)
#if defined(SENTRY_PLATFORM_UNIX)
    unlink(envelope_path);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    wchar_t *wenvelope_unlink = sentry__string_to_wstr(envelope_path);
    if (wenvelope_unlink) {
        _wunlink(wenvelope_unlink);
        sentry_free(wenvelope_unlink);
    }
#endif

cleanup:
    // Send the staged session-replay envelope same-session, enriched from the
    // crash event (`<run>/__sentry-event`) so it shares the crash's
    // tags/contexts/trace. Only flush when the crash itself was delivered:
    // `cleanup` is also reached via `goto` on error paths where the crash was
    // never captured, and flushing there would consume (and delete) the staged
    // replay for a crash that never arrived.
    if (crash_captured && options && options->transport
        && sentry__session_replay_has_pending(options)) {
        sentry_value_t crash_event = sentry_value_new_null();
        if (ev_path) {
            size_t ev_len = 0;
            char *ev_json = sentry__path_read_to_buffer(ev_path, &ev_len);
            if (ev_json) {
                crash_event = sentry__value_from_json(ev_json, ev_len);
                sentry_free(ev_json);
            }
        }
        sentry__session_replay_flush_pending(
            options, options->transport, crash_event);
        sentry_value_decref(crash_event);
    }

    // Send all other envelopes from run folder (logs, etc.) before cleanup
    if (run_folder && options && options->transport && options->run) {
        SENTRY_DEBUG("Checking for additional envelopes in run folder");
        sentry_pathiter_t *piter = sentry__path_iter_directory(run_folder);
        if (piter) {
            SENTRY_DEBUG("Iterating run folder for envelope files");
            const sentry_path_t *file_path;
            int envelope_count = 0;
            while ((file_path = sentry__pathiter_next(piter)) != NULL) {
                // Check if this is an envelope file (ends with .envelope)
                const char *path_str = file_path->path;
                size_t len = strlen(path_str);
                if (len > 9 && strcmp(path_str + len - 9, ".envelope") == 0) {
                    SENTRY_DEBUGF(
                        "Sending envelope from run folder: %s", path_str);
                    sentry_envelope_t *run_envelope
                        = sentry__envelope_from_path(file_path);
                    if (run_envelope) {
                        sentry__capture_envelope(
                            options->transport, run_envelope, options);
                        envelope_count++;
                    } else {
                        SENTRY_WARNF("Failed to load envelope: %s", path_str);
                    }
                }
            }
            SENTRY_DEBUGF(
                "Sent %d additional envelopes from run folder", envelope_count);
            sentry__pathiter_free(piter);
        } else {
            SENTRY_DEBUG("Could not iterate run folder");
        }
    } else {
        SENTRY_DEBUG("No run folder or transport for additional envelopes");
    }

    // Clean up the entire run folder (contains breadcrumbs, etc.)
    if (run_folder) {
        SENTRY_DEBUG("Cleaning up run folder");
        sentry__path_remove_all(run_folder);
        sentry__path_free(run_folder);
    }
    sentry__path_free(ev_path);

    // Release and clean up the lock file
    if (run_lock) {
        sentry__filelock_unlock(run_lock);
        sentry__filelock_free(run_lock);
    }
    SENTRY_DEBUG("Cleaned up crash run folder and lock file");

    SENTRY_DEBUG("Crash processing completed successfully");

done:
    SENTRY_DEBUG("Processing crash - END");
    SENTRY_DEBUG("Crash processing complete");
    return crash_captured;
}

/**
 * Check if parent process is still alive
 */
static bool
is_parent_alive(sentry_process_handle_t parent)
{
#if defined(SENTRY_PLATFORM_UNIX)
    // Send signal 0 to check if process exists
    return kill(parent, 0) == 0 || errno != ESRCH;
#elif defined(SENTRY_PLATFORM_WINDOWS)
    if (!parent) {
        return false; // Process doesn't exist or can't be accessed
    }
    // Check if process has exited
    return WaitForSingleObject(parent, 0) == WAIT_TIMEOUT;
#endif
}

/**
 * Custom logger function that writes to a file
 * Used by the daemon to log its activity
 */
static void
daemon_file_logger(
    sentry_level_t level, const char *message, va_list args, void *userdata)
{
    FILE *log_file = (FILE *)userdata;
    if (!log_file) {
        return;
    }

    // Get current timestamp
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[SENTRY_CRASH_TIMESTAMP_SIZE];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    // Get level description from Sentry's formatter
    const char *level_str = sentry__logger_describe(level);

    // Write log entry
    fprintf(log_file, "[%s] %s", timestamp, level_str);
    vfprintf(log_file, message, args);
    fprintf(log_file, "\n");
    fflush(log_file); // Flush immediately to ensure logs are written
}

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
int
sentry__crash_daemon_main(
    pid_t app_pid, uint64_t app_tid, int notify_eventfd, int ready_eventfd)
#elif defined(SENTRY_PLATFORM_MACOS)
int
sentry__crash_daemon_main(pid_t app_pid, uint64_t app_tid, int notify_pipe_read,
    int ready_pipe_write, int shm_fd)
#elif defined(SENTRY_PLATFORM_WINDOWS)
int
sentry__crash_daemon_main(pid_t app_pid, uint64_t app_tid, HANDLE event_handle,
    HANDLE ready_event_handle)
#endif
{
    // Initialize IPC first (attach to shared memory created by parent)
    // We need this to get the database path for logging
#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    sentry_crash_ipc_t *ipc = sentry__crash_ipc_init_daemon(
        app_pid, app_tid, notify_eventfd, ready_eventfd);
#elif defined(SENTRY_PLATFORM_MACOS)
    sentry_crash_ipc_t *ipc = sentry__crash_ipc_init_daemon(
        app_pid, app_tid, notify_pipe_read, ready_pipe_write, shm_fd);
#elif defined(SENTRY_PLATFORM_WINDOWS)
    sentry_crash_ipc_t *ipc = sentry__crash_ipc_init_daemon(
        app_pid, app_tid, event_handle, ready_event_handle);
#endif
    if (!ipc) {
        return 1;
    }

    // Set up logging to file for daemon BEFORE redirecting streams
    // Use same naming scheme as shared memory (PID ^ TID hash) to handle
    // multiple threads in same process
    char log_path[SENTRY_CRASH_MAX_PATH];
    FILE *log_file = NULL;
    uint32_t id = (uint32_t)((app_pid ^ (app_tid & 0xFFFFFFFF)) & 0xFFFFFFFF);

#if defined(SENTRY_PLATFORM_WINDOWS)
    // On Windows, convert UTF-8 path to wide characters for proper file
    // handling
    int log_path_len = snprintf(log_path, sizeof(log_path),
        "%s\\sentry-daemon-%08x.log", ipc->shmem->database_path, id);

    if (log_path_len > 0 && log_path_len < (int)sizeof(log_path)) {
        wchar_t *wlog_path = sentry__string_to_wstr(log_path);
        if (wlog_path) {
            log_file = _wfopen(wlog_path, L"w");
            sentry_free(wlog_path);
        }
    }
#else
    int log_path_len = snprintf(log_path, sizeof(log_path),
        "%s/sentry-daemon-%08x.log", ipc->shmem->database_path, id);

    if (log_path_len > 0 && log_path_len < (int)sizeof(log_path)) {
        log_file = fopen(log_path, "w");
    }
#endif

    if (log_file) {
        // Disable buffering for immediate writes
        setvbuf(log_file, NULL, _IONBF, 0);

        // Set up Sentry logger to write to file
        // Use log level from parent's debug setting
        sentry_level_t log_level = ipc->shmem->debug_enabled
            ? SENTRY_LEVEL_DEBUG
            : SENTRY_LEVEL_INFO;
        sentry_logger_t file_logger = { .logger_func = daemon_file_logger,
            .logger_data = log_file,
            .logger_level = log_level };
        sentry__logger_set_global(file_logger);
        sentry__logger_enable();

        SENTRY_DEBUG("=== Daemon starting ===");
        SENTRY_DEBUGF("App PID: %lu", (unsigned long)app_pid);
        SENTRY_DEBUGF("Database path: %s", ipc->shmem->database_path);
    }

#if defined(SENTRY_PLATFORM_UNIX)
    // Close standard streams to avoid interfering with parent
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Open /dev/null for std streams
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
    }
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // On Windows, redirect stdin/stdout to NUL
    // But redirect stderr to the log file so fprintf(stderr) appears in the log
    (void)freopen("NUL", "r", stdin);
    (void)freopen("NUL", "w", stdout);

    (void)freopen("NUL", "w", stderr);
#endif

    // Initialize Sentry options for daemon (reuses all SDK infrastructure)
    // Options are passed explicitly to all functions, no global state
    sentry_options_t *options = sentry_options_new();
    if (!options) {
        SENTRY_ERROR("sentry_options_new() failed");
        if (log_file) {
            fclose(log_file);
        }
        return 1;
    }

    // Use debug logging and screenshot settings from parent process
    sentry_options_set_debug(options, ipc->shmem->debug_enabled);
    options->attach_screenshot = ipc->shmem->attach_screenshot;
    options->attach_session_replay = ipc->shmem->attach_session_replay;
    options->session_replay_duration = ipc->shmem->session_replay_duration;
    options->cache_keep = (sentry_cache_keep_t)ipc->shmem->cache_keep;
    options->enable_large_attachments = ipc->shmem->enable_large_attachments;
    options->http_retry = false;
    options->shutdown_timeout = ipc->shmem->shutdown_timeout;
    options->transfer_timeout = ipc->shmem->transfer_timeout;

    // Set custom logger that writes to file
    if (log_file) {
        sentry_options_set_logger(options, daemon_file_logger, log_file);
    }

    // Set DSN if configured
    if (ipc->shmem->dsn[0] != '\0') {
        SENTRY_DEBUGF("Setting DSN: %s", ipc->shmem->dsn);
        sentry_options_set_dsn(options, ipc->shmem->dsn);
    } else {
        SENTRY_DEBUG("No DSN configured");
    }

    // Set transport configuration from parent process options
    if (ipc->shmem->ca_certs[0] != '\0') {
        SENTRY_DEBUGF("Setting CA certs: %s", ipc->shmem->ca_certs);
        sentry_options_set_ca_certs(options, ipc->shmem->ca_certs);
    }

    if (ipc->shmem->proxy[0] != '\0') {
        SENTRY_DEBUGF("Setting proxy: %s", ipc->shmem->proxy);
        sentry_options_set_proxy(options, ipc->shmem->proxy);
    }

    if (ipc->shmem->user_agent[0] != '\0') {
        SENTRY_DEBUGF("Setting user agent: %s", ipc->shmem->user_agent);
        sentry_free(options->user_agent);
        options->user_agent = sentry__string_clone(ipc->shmem->user_agent);
    }

    // Create run with database path
    SENTRY_DEBUG("Creating run with database path");
    sentry_path_t *db_path = sentry__path_from_str(ipc->shmem->database_path);
    if (db_path) {
        options->run = sentry__run_new(db_path);
        if (options->run) {
            options->run->require_user_consent
                = ipc->shmem->require_user_consent;
        }
        sentry__path_free(db_path);
    }

    // Set external crash reporter if configured
    if (ipc->shmem->external_reporter_path[0] != '\0') {
        SENTRY_DEBUGF("Setting external reporter: %s",
            ipc->shmem->external_reporter_path);
        sentry_path_t *reporter
            = sentry__path_from_str(ipc->shmem->external_reporter_path);
        if (reporter) {
            options->external_crash_reporter = reporter;
        }
    }

    // Transport is already initialized by sentry_options_new(), just start it
    if (options->transport) {
        SENTRY_DEBUG("Starting transport");
        sentry__transport_startup(options->transport, options);
        // Set http_retry after transport startup to keep daemon-side retry
        // polling disabled, while letting capture cache consent-revoked
        // envelopes in retry format for the app to send on restart.
        options->http_retry = ipc->shmem->http_retry;
    } else {
        SENTRY_WARN("No transport available");
    }

    SENTRY_DEBUG("Daemon options fully initialized");

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    // Use the inherited eventfd from parent
    ipc->notify_fd = notify_eventfd;
    ipc->ready_fd = ready_eventfd;
#elif defined(SENTRY_PLATFORM_WINDOWS)
    // On Windows, event handle is already opened by name in init_daemon
    // Don't overwrite it with the parent's handle (handles are per-process)
    (void)event_handle;
    (void)ready_event_handle;
#endif

#if defined(SENTRY_PLATFORM_WINDOWS)
    // Open handle to parent process with SYNCHRONIZE for WaitForSingleObject
    // in is_parent_alive(). Once on startup because OpenProcess fails on OOM.
    ipc->parent_handle = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)app_pid);
    if (!ipc->parent_handle) {
        SENTRY_WARNF(
            "Failed to open parent process handle: %lu", GetLastError());
    }
#else
    ipc->parent_handle = app_pid;
#endif

    // Signal to parent that daemon is ready
    SENTRY_DEBUG("Signaling ready to parent");
    sentry__crash_ipc_signal_ready(ipc);

    SENTRY_DEBUG("Entering main loop");

    // Daemon main loop
    bool crash_processed = false;
    while (true) {
        // Wait for crash notification (with timeout to check parent health)
        bool wait_result
            = sentry__crash_ipc_wait(ipc, SENTRY_CRASH_DAEMON_WAIT_TIMEOUT_MS);
        if (wait_result) {
            // Crash occurred!
            SENTRY_DEBUG("Event signaled, checking crash state");

            // Retry reading state with delays to handle CPU cache coherency
            // issues Between processes, cache lines may take time to
            // invalidate/sync
            long state = sentry__atomic_fetch(&ipc->shmem->state);
            if (state == SENTRY_CRASH_STATE_CRASHED && !crash_processed) {
                SENTRY_DEBUG("Crash notification received, processing");
                bool crash_captured = sentry__process_crash(options, ipc);
                crash_processed = true;

                if (crash_captured
                    && ipc->shmem->crash_upload_mode
                        == SENTRY_CRASH_UPLOAD_MODE_ASYNC) {
                    // Crash data is durable after processing returns;
                    // remaining daemon work does not require the crashed
                    // process.
                    SENTRY_DEBUG(
                        "Crash captured, allowing app process to exit");
                    sentry__atomic_store(
                        &ipc->shmem->state, SENTRY_CRASH_STATE_CAPTURED);
                }

                // After processing crash, exit regardless of parent state
                // (parent has likely already exited after re-raising signal)
                SENTRY_DEBUG("Crash processed, daemon exiting");
                break;
            }
            // If crash already processed, just ignore spurious notifications
            SENTRY_DEBUG("Spurious notification or already processed");
        }

        // Check if parent is still alive (only if no crash processed yet)
        if (!crash_processed && !is_parent_alive(ipc->parent_handle)) {
            SENTRY_DEBUG("Parent process exited without crash");
            break;
        }
    }

    SENTRY_DEBUG("Daemon exiting");

    // Cleanup
    if (options) {
        size_t dumped_envelopes = 0;
        if (options->transport) {
            // Wait for the configured SDK shutdown timeout to send pending
            // envelopes (crash envelope + logs envelope, etc.).
            int rv = sentry__transport_shutdown(
                options->transport, options->shutdown_timeout);
            if (rv != 0) {
                SENTRY_WARN("transport did not shut down cleanly");
            }
            dumped_envelopes = sentry__transport_dump_queue(
                options->transport, options->run);
            if (rv == 0 && !dumped_envelopes && options->run) {
                sentry__run_clean(options->run, true);
            }
        }
        sentry_options_free(options);
    }
    if (crash_processed) {
        // Mark as done
        SENTRY_DEBUG("Marking crash state as DONE");
        sentry__atomic_store(&ipc->shmem->state, SENTRY_CRASH_STATE_DONE);
    }
    if (crash_processed || !is_parent_alive(ipc->parent_handle)) {
        sentry__crash_ipc_unlink(ipc);
    }
    sentry__crash_ipc_free(ipc);

    // Close log file
    if (log_file) {
        fclose(log_file);
    }

    return 0;
}

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
pid_t
sentry__crash_daemon_start(pid_t app_pid, uint64_t app_tid, int notify_eventfd,
    int ready_eventfd, const char *handler_path)
#elif defined(SENTRY_PLATFORM_MACOS)
pid_t
sentry__crash_daemon_start(pid_t app_pid, uint64_t app_tid,
    int notify_pipe_read, int ready_pipe_write, int shm_fd,
    const char *handler_path)
#elif defined(SENTRY_PLATFORM_WINDOWS)
pid_t
sentry__crash_daemon_start(pid_t app_pid, uint64_t app_tid, HANDLE event_handle,
    HANDLE ready_event_handle, const char *handler_path)
#endif
{
#if defined(SENTRY_PLATFORM_MACOS)
    // macOS: Use posix_spawn instead of fork+exec for App Sandbox
    // compatibility. posix_spawn is Apple's recommended API and works correctly
    // in sandboxed processes, unlike fork() which can have issues with sandbox
    // inheritance.

    // Resolve daemon path
    char daemon_path[SENTRY_CRASH_MAX_PATH];
    if (!sentry__string_empty(handler_path)) {
        strncpy(daemon_path, handler_path, sizeof(daemon_path) - 1);
        daemon_path[sizeof(daemon_path) - 1] = '\0';
    } else {
        char exe_path[SENTRY_CRASH_MAX_PATH];
        uint32_t exe_size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &exe_size) != 0) {
            SENTRY_WARN("Failed to get executable path for daemon");
            return -1;
        }
        const char *slash = strrchr(exe_path, '/');
        if (!slash
            || (size_t)(slash - exe_path + 1) + strlen("sentry-crash")
                >= sizeof(daemon_path)) {
            SENTRY_WARN("Daemon path too long");
            return -1;
        }
        size_t dir_len = (size_t)(slash - exe_path + 1);
        memcpy(daemon_path, exe_path, dir_len);
        strcpy(daemon_path + dir_len, "sentry-crash");
    }

    // Build argument strings (6 args: pid, tid, notify_fd, ready_fd, shm_fd)
    char pid_str[32], tid_str[32], notify_str[32], ready_str[32], shm_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", (int)app_pid);
    snprintf(tid_str, sizeof(tid_str), "%" PRIx64, app_tid);
    snprintf(notify_str, sizeof(notify_str), "%d", notify_pipe_read);
    snprintf(ready_str, sizeof(ready_str), "%d", ready_pipe_write);
    snprintf(shm_str, sizeof(shm_str), "%d", shm_fd);

    char *spawn_argv[] = { "sentry-crash", pid_str, tid_str, notify_str,
        ready_str, shm_str, NULL };

    // Set up posix_spawn attributes
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // POSIX_SPAWN_SETSID: create new session (like setsid() after fork)
    // POSIX_SPAWN_CLOEXEC_DEFAULT: close all fds except explicitly inherited
    short spawn_flags = POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT;
    posix_spawnattr_setflags(&attr, spawn_flags);

    // Explicitly inherit only the fds the daemon needs
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_addinherit_np(&file_actions, notify_pipe_read);
    posix_spawn_file_actions_addinherit_np(&file_actions, ready_pipe_write);
    posix_spawn_file_actions_addinherit_np(&file_actions, shm_fd);
    // Open /dev/null on stdin/stdout/stderr so the daemon starts with valid
    // standard fds. Without this, POSIX_SPAWN_CLOEXEC_DEFAULT closes them,
    // and the first fopen() in the daemon would get fd 0, which the daemon's
    // own close(STDIN_FILENO) would then destroy.
    // Skip if an IPC fd occupies that slot (e.g. caller closed stdin before
    // sentry_init), to avoid clobbering it with /dev/null.
    int std_fds[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    int std_modes[3] = { O_RDONLY, O_WRONLY, O_WRONLY };
    for (int i = 0; i < 3; i++) {
        if (std_fds[i] != notify_pipe_read && std_fds[i] != ready_pipe_write
            && std_fds[i] != shm_fd) {
            posix_spawn_file_actions_addopen(
                &file_actions, std_fds[i], "/dev/null", std_modes[i], 0);
        }
    }

    pid_t daemon_pid;
    int spawn_result = posix_spawn(&daemon_pid, daemon_path, &file_actions,
        &attr, spawn_argv, *_NSGetEnviron());

    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);

    if (spawn_result != 0) {
        SENTRY_WARNF("posix_spawn failed for %s: %s", daemon_path,
            strerror(spawn_result));
        return -1;
    }

    return daemon_pid;

#elif defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    // Linux: Use fork+exec
    pid_t daemon_pid = fork();

    if (daemon_pid < 0) {
        SENTRY_WARN("Failed to fork daemon process");
        return -1;
    } else if (daemon_pid == 0) {
        // Child process - exec sentry-crash
        setsid();

        // Clear FD_CLOEXEC on notify and ready fds so they survive exec
        int notify_flags = fcntl(notify_eventfd, F_GETFD);
        if (notify_flags != -1) {
            fcntl(notify_eventfd, F_SETFD, notify_flags & ~FD_CLOEXEC);
        }
        int ready_flags = fcntl(ready_eventfd, F_GETFD);
        if (ready_flags != -1) {
            fcntl(ready_eventfd, F_SETFD, ready_flags & ~FD_CLOEXEC);
        }

        // Convert arguments to strings for exec
        char pid_str[32], tid_str[32], notify_str[32], ready_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)app_pid);
        snprintf(tid_str, sizeof(tid_str), "%" PRIx64, app_tid);
        snprintf(notify_str, sizeof(notify_str), "%d", notify_eventfd);
        snprintf(ready_str, sizeof(ready_str), "%d", ready_eventfd);

        char *argv[]
            = { "sentry-crash", pid_str, tid_str, notify_str, ready_str, NULL };

        if (!sentry__string_empty(handler_path)) {
            execv(handler_path, argv);
        } else {
            char exe_path[SENTRY_CRASH_MAX_PATH];
            char daemon_exec_path[SENTRY_CRASH_MAX_PATH];

            ssize_t exe_len
                = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
            if (exe_len > 0) {
                exe_path[exe_len] = '\0';
                const char *slash = strrchr(exe_path, '/');
                if (slash) {
                    size_t dir_len = (size_t)(slash - exe_path + 1);
                    if (dir_len + strlen("sentry-crash")
                        < sizeof(daemon_exec_path)) {
                        memcpy(daemon_exec_path, exe_path, dir_len);
                        strcpy(daemon_exec_path + dir_len, "sentry-crash");
                        execv(daemon_exec_path, argv);
                    }
                }
            }
        }

        // exec failed - exit with error
        perror("Failed to exec sentry-crash");
        _exit(1);
    }

    // Parent process - return daemon PID
    return daemon_pid;

#elif defined(SENTRY_PLATFORM_WINDOWS)
    // On Windows, create a separate daemon process using CreateProcess
    // Spawn the sentry-crash.exe executable

    wchar_t daemon_path_w[SENTRY_CRASH_MAX_PATH];

    // If handler_path was explicitly set via options, use it directly
    if (!sentry__string_empty(handler_path)) {
        wchar_t *wpath = sentry__string_to_wstr(handler_path);
        if (wpath) {
            wcsncpy(daemon_path_w, wpath, SENTRY_CRASH_MAX_PATH - 1);
            daemon_path_w[SENTRY_CRASH_MAX_PATH - 1] = L'\0';
            sentry_free(wpath);
        } else {
            SENTRY_WARN("Failed to convert handler_path to wide string");
            return (pid_t)-1;
        }
    } else {
        // Try to find sentry-crash.exe in the same directory as the current
        // executable
        wchar_t exe_dir[SENTRY_CRASH_MAX_PATH];
        DWORD len = GetModuleFileNameW(NULL, exe_dir, SENTRY_CRASH_MAX_PATH);
        if (len == 0 || len >= SENTRY_CRASH_MAX_PATH) {
            SENTRY_WARN("Failed to get current executable path");
            return (pid_t)-1;
        }

        // Remove filename to get directory
        wchar_t *last_slash = wcsrchr(exe_dir, L'\\');
        if (last_slash) {
            *(last_slash + 1) = L'\0'; // Keep the trailing backslash
        }

        // Build full path to sentry-crash.exe
        int path_len = _snwprintf(daemon_path_w, SENTRY_CRASH_MAX_PATH,
            L"%ssentry-crash.exe", exe_dir);
        if (path_len < 0 || path_len >= SENTRY_CRASH_MAX_PATH) {
            SENTRY_WARN("Daemon path too long");
            return (pid_t)-1;
        }
    }

    // Log the daemon path we're trying to launch for debugging
    char *daemon_path_utf8 = sentry__string_from_wstr(daemon_path_w);
    if (daemon_path_utf8) {
        SENTRY_DEBUGF("Attempting to launch daemon: %s", daemon_path_utf8);
        sentry_free(daemon_path_utf8);
    }

    // Build command line: sentry-crash.exe <app_pid> <app_tid> <event_handle>
    // <ready_event_handle>
    wchar_t cmd_line[SENTRY_CRASH_MAX_PATH + 128];
    int cmd_len = _snwprintf(cmd_line, sizeof(cmd_line) / sizeof(wchar_t),
        L"\"%s\" %lu %llx %llu %llu", daemon_path_w, (unsigned long)app_pid,
        (unsigned long long)app_tid,
        (unsigned long long)(uintptr_t)event_handle,
        (unsigned long long)(uintptr_t)ready_event_handle);

    if (cmd_len < 0 || cmd_len >= (int)(sizeof(cmd_line) / sizeof(wchar_t))) {
        SENTRY_WARN("Command line too long for daemon spawn");
        return (pid_t)-1;
    }

    // Prepare process creation structures
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    // Hide console window for daemon
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    // Create the daemon process
    if (!CreateProcessW(NULL, // Application name (use command line)
            cmd_line, // Command line
            NULL, // Process security attributes
            NULL, // Thread security attributes
            TRUE, // Inherit handles (for event_handle)
            CREATE_NO_WINDOW | DETACHED_PROCESS, // Creation flags
            NULL, // Environment
            NULL, // Current directory
            &si, // Startup info
            &pi)) { // Process information
        DWORD error = GetLastError();
        char *daemon_path_err = sentry__string_from_wstr(daemon_path_w);
        if (daemon_path_err) {
            SENTRY_WARNF("Failed to create daemon process at '%s': Error %lu%s",
                daemon_path_err, error,
                error == 2       ? " (File not found)"
                    : error == 3 ? " (Path not found)"
                                 : "");
            sentry_free(daemon_path_err);
        } else {
            SENTRY_WARNF("Failed to create daemon process: %lu", error);
        }
        return (pid_t)-1;
    }

    // Close thread handle (we don't need it)
    CloseHandle(pi.hThread);

    // Close process handle (daemon is independent)
    CloseHandle(pi.hProcess);

    // Return daemon process ID
    return pi.dwProcessId;
#endif
}

// When built as standalone executable, provide main entry point
#ifdef SENTRY_CRASH_DAEMON_STANDALONE

#    if defined(SENTRY_PLATFORM_XBOX)
// Thin wrappers provided by sentry-xbox; the daemon doesn't link XGameRuntime
// directly so these indirections keep all Xbox-platform coupling on that side.
extern bool sentry__xbox_game_runtime_initialize(void);
extern void sentry__xbox_game_runtime_uninitialize(void);

extern bool sentry__xbox_ensure_network_initialized(void);

static DWORD WINAPI
sentry__xbox_network_prewarm_thread_proc(LPVOID param)
{
    (void)param;
    (void)sentry__xbox_ensure_network_initialized();
    return 0;
}
#    endif

int
main(int argc, char **argv)
{
    // Expected arguments:
    //   Linux:  <app_pid> <app_tid> <notify_handle> <ready_handle>
    //   macOS:  <app_pid> <app_tid> <notify_handle> <ready_handle> <shm_fd>
#    if defined(SENTRY_PLATFORM_MACOS)
    if (argc < 6) {
        fprintf(stderr,
            "Usage: sentry-crash <app_pid> <app_tid> <notify_pipe> "
            "<ready_pipe> <shm_fd>\n");
        return 1;
    }
#    else
    if (argc < 5) {
        fprintf(stderr,
            "Usage: sentry-crash <app_pid> <app_tid> <notify_handle> "
            "<ready_handle>\n");
        return 1;
    }
#    endif

    // Parse arguments
    pid_t app_pid = (pid_t)strtoul(argv[1], NULL, 10);
    uint64_t app_tid = strtoull(argv[2], NULL, 16);

#    if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    int notify_eventfd = atoi(argv[3]);
    int ready_eventfd = atoi(argv[4]);
    return sentry__crash_daemon_main(
        app_pid, app_tid, notify_eventfd, ready_eventfd);
#    elif defined(SENTRY_PLATFORM_MACOS)
    int notify_pipe_read = atoi(argv[3]);
    int ready_pipe_write = atoi(argv[4]);
    int shm_fd_arg = atoi(argv[5]);
    return sentry__crash_daemon_main(
        app_pid, app_tid, notify_pipe_read, ready_pipe_write, shm_fd_arg);
#    elif defined(SENTRY_PLATFORM_WINDOWS)
    unsigned long long event_handle_val = strtoull(argv[3], NULL, 10);
    unsigned long long ready_event_val = strtoull(argv[4], NULL, 10);
    HANDLE event_handle = (HANDLE)(uintptr_t)event_handle_val;
    HANDLE ready_event_handle = (HANDLE)(uintptr_t)ready_event_val;

#        if defined(SENTRY_PLATFORM_XBOX)
    // Required before any XNetworking call the transport makes at
    // crash-upload time.
    if (!sentry__xbox_game_runtime_initialize()) {
        return 1;
    }

    HANDLE network_prewarm_thread = CreateThread(
        NULL, 0, sentry__xbox_network_prewarm_thread_proc, NULL, 0, NULL);
#        endif

    int rv = sentry__crash_daemon_main(
        app_pid, app_tid, event_handle, ready_event_handle);

#        if defined(SENTRY_PLATFORM_XBOX)
    if (network_prewarm_thread) {
        WaitForSingleObject(network_prewarm_thread, INFINITE);
        CloseHandle(network_prewarm_thread);
    }

    sentry__xbox_game_runtime_uninitialize();
#        endif
    return rv;
#    else
    fprintf(stderr, "Platform not supported\n");
    return 1;
#    endif
}

#endif // SENTRY_CRASH_DAEMON_STANDALONE
