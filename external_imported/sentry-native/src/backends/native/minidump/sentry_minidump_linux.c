#include "sentry_boot.h"

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
#    include <dirent.h>
#    include <elf.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <link.h> // ElfW, struct r_debug, struct link_map (DSO debug stream)
#    include <stdbool.h>
#    include <stdio.h>
#    include <stdlib.h>
#    include <string.h>
#    include <sys/ptrace.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <sys/uio.h>
#    include <sys/user.h>
#    include <sys/utsname.h>
#    include <sys/wait.h>
#    include <time.h>
#    include <unistd.h>

#    include "sentry_alloc.h"
#    include "sentry_elf.h"
#    include "sentry_logger.h"
#    include "sentry_minidump_common.h"
#    include "sentry_minidump_format.h"
#    include "sentry_minidump_indirect.h"
#    include "sentry_minidump_writer.h"

// NT_PRSTATUS is defined in linux/elf.h but we can't include that
// because it conflicts with elf.h. Define it here if not available.
#    ifndef NT_PRSTATUS
#        define NT_PRSTATUS 1
#    endif

// CodeView record format for ELF modules with Build ID
// CV signature: 'BpEL' (Breakpad ELF) - compatible with Breakpad/Crashpad
#    define CV_SIGNATURE_ELF 0x4270454c // "BpEL" in little-endian

typedef struct {
    uint32_t cv_signature; // 'BpEL' (0x4270454c)
    uint8_t build_id[1]; // Variable length Build ID from ELF .note.gnu.build-id
                         // Typically 20 bytes (SHA-1) but can vary
} __attribute__((packed)) cv_info_elf_t;

#    if defined(__aarch64__)
// ARM64 signal context structures for accessing FPSIMD state
#        define FPSIMD_MAGIC 0x46508001

// Only define these if not already provided by system headers
// (on musl, defined in <bits/signal.h> instead).
#        if !defined(__ASM_SIGCONTEXT_H) && defined(__GLIBC__)
// Base header for context blocks in __reserved
struct _aarch64_ctx {
    uint32_t magic;
    uint32_t size;
};

// FPSIMD context containing NEON/FP registers
struct fpsimd_context {
    struct _aarch64_ctx head;
    uint32_t fpsr;
    uint32_t fpcr;
    __uint128_t vregs[32];
};
#        endif
#    endif

// Use process_vm_readv to read memory from crashed process
#    include <sys/uio.h>

// Use shared constants from crash context
#    include "../sentry_crash_context.h"

/**
 * Memory mapping from /proc/[pid]/maps
 */
typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t offset;
    char permissions[5]; // "rwxp"
    char name[256];
} memory_mapping_t;

/**
 * Minidump writer context
 * Note: fd and current_offset must be first to match minidump_writer_base_t
 */
typedef struct {
    // Base fields (must match minidump_writer_base_t layout)
    int fd;
    uint64_t current_offset;

    // Linux-specific fields
    const sentry_crash_context_t *crash_ctx;

    // Memory mappings
    memory_mapping_t mappings[SENTRY_CRASH_MAX_MAPPINGS];
    size_t mapping_count;

    // Threads
    pid_t tids[SENTRY_CRASH_MAX_THREADS];
    char thread_names[SENTRY_CRASH_MAX_THREADS]
                     [16]; // From /proc/[pid]/task/[tid]/comm
    size_t thread_count;

    // Thread-stack memory descriptors recorded as we write the ThreadList
    // stream. We replay them into MemoryListStream so consumers (notably
    // LLDB's ProcessMinidump on Linux) can find stack bytes by virtual
    // address — without this LLDB knows the stack region exists (from
    // LinuxMaps) but cannot read its contents and unwinding stops after
    // frame 0. Breakpad's MinidumpWriter does the equivalent via
    // memory_blocks_.push_back(thread->stack).
    //
    // thread_stack_tids[i] is the thread id whose stack is in
    // thread_stacks[i] — needed by the SMART-mode indirect-memory walker so
    // it can find the matching ucontext for register scanning.
    minidump_memory_descriptor_t thread_stacks[SENTRY_CRASH_MAX_THREADS];
    pid_t thread_stack_tids[SENTRY_CRASH_MAX_THREADS];
    size_t thread_stack_count;

    // Ptrace state
    bool ptrace_attached;
} minidump_writer_t;

/**
 * Attach to process using ptrace (must be called once before reading memory)
 */
static bool
ptrace_attach_process(minidump_writer_t *writer)
{
    if (writer->ptrace_attached) {
        return true;
    }

    // Attach to the crashed thread specifically (not just the process PID).
    // On Linux, ptrace operates on individual threads (LWPs). Attaching to
    // crashed_pid would only attach to the main thread, causing
    // PTRACE_GETFPREGS to fail when the crash occurs on a different thread.
    pid_t tid = writer->crash_ctx->crashed_tid;
    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) != 0) {
        SENTRY_WARNF("ptrace(PTRACE_ATTACH) failed for TID %d: %s", tid,
            strerror(errno));
        return false;
    }

    // Wait for thread to stop
    int status;
    if (waitpid(tid, &status, __WALL) < 0) {
        SENTRY_WARNF("waitpid after PTRACE_ATTACH failed for TID %d: %s", tid,
            strerror(errno));
        ptrace(PTRACE_DETACH, tid, NULL, NULL);
        return false;
    }

    writer->ptrace_attached = true;
    SENTRY_DEBUGF("Successfully attached to thread %d via ptrace", tid);
    return true;
}

/**
 * Get FPU state via ptrace for x86_64
 * Must be called while thread is attached
 */
#    if defined(__x86_64__)
static bool
ptrace_get_fpregs(pid_t tid, struct user_fpregs_struct *fpregs)
{
    if (ptrace(PTRACE_GETFPREGS, tid, NULL, fpregs) == 0) {
        SENTRY_DEBUGF(
            "Thread %d: successfully captured FPU state via ptrace", tid);
        return true;
    } else {
        SENTRY_DEBUGF(
            "Thread %d: PTRACE_GETFPREGS failed: %s", tid, strerror(errno));
        return false;
    }
}
#    endif

/**
 * Get thread registers via ptrace (for non-crashed threads)
 * Returns true if registers were successfully captured
 */
static bool
ptrace_get_thread_registers(pid_t tid, ucontext_t *uctx)
{
    // Attach to the specific thread
    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) != 0) {
        SENTRY_DEBUGF("ptrace(PTRACE_ATTACH) failed for thread %d: %s", tid,
            strerror(errno));
        return false;
    }

    // Wait for thread to stop
    int status;
    if (waitpid(tid, &status, __WALL) < 0) {
        SENTRY_DEBUGF("waitpid after PTRACE_ATTACH failed for thread %d: %s",
            tid, strerror(errno));
        ptrace(PTRACE_DETACH, tid, NULL, NULL);
        return false;
    }

    // Get general purpose registers
    bool success = false;

#    if defined(__x86_64__)
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
        // Map to ucontext_t format
        uctx->uc_mcontext.gregs[REG_R8] = regs.r8;
        uctx->uc_mcontext.gregs[REG_R9] = regs.r9;
        uctx->uc_mcontext.gregs[REG_R10] = regs.r10;
        uctx->uc_mcontext.gregs[REG_R11] = regs.r11;
        uctx->uc_mcontext.gregs[REG_R12] = regs.r12;
        uctx->uc_mcontext.gregs[REG_R13] = regs.r13;
        uctx->uc_mcontext.gregs[REG_R14] = regs.r14;
        uctx->uc_mcontext.gregs[REG_R15] = regs.r15;
        uctx->uc_mcontext.gregs[REG_RDI] = regs.rdi;
        uctx->uc_mcontext.gregs[REG_RSI] = regs.rsi;
        uctx->uc_mcontext.gregs[REG_RBP] = regs.rbp;
        uctx->uc_mcontext.gregs[REG_RBX] = regs.rbx;
        uctx->uc_mcontext.gregs[REG_RDX] = regs.rdx;
        uctx->uc_mcontext.gregs[REG_RAX] = regs.rax;
        uctx->uc_mcontext.gregs[REG_RCX] = regs.rcx;
        uctx->uc_mcontext.gregs[REG_RSP] = regs.rsp;
        uctx->uc_mcontext.gregs[REG_RIP] = regs.rip;
        uctx->uc_mcontext.gregs[REG_EFL] = regs.eflags;
        uctx->uc_mcontext.gregs[REG_CSGSFS]
            = (regs.cs & 0xffff) | ((regs.gs & 0xffff) << 16);
        uctx->uc_mcontext.gregs[REG_ERR] = 0;
        uctx->uc_mcontext.gregs[REG_TRAPNO] = 0;
        uctx->uc_mcontext.gregs[REG_OLDMASK] = 0;
        uctx->uc_mcontext.gregs[REG_CR2] = 0;
        success = true;
        SENTRY_DEBUGF("Thread %d: captured registers via ptrace, SP=0x%llx",
            tid, (unsigned long long)regs.rsp);
    } else {
        SENTRY_DEBUGF("ptrace(PTRACE_GETREGS) failed for thread %d: %s", tid,
            strerror(errno));
    }
#    elif defined(__aarch64__)
    struct user_regs_struct regs;
    struct iovec iov;
    iov.iov_base = &regs;
    iov.iov_len = sizeof(regs);
    if (ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRSTATUS, &iov) == 0) {
        // Map to ucontext_t format
        for (int i = 0; i < 31; i++) {
            uctx->uc_mcontext.regs[i] = regs.regs[i];
        }
        uctx->uc_mcontext.sp = regs.sp;
        uctx->uc_mcontext.pc = regs.pc;
        uctx->uc_mcontext.pstate = regs.pstate;
        success = true;
        SENTRY_DEBUGF("Thread %d: captured registers via ptrace, SP=0x%llx",
            tid, (unsigned long long)regs.sp);
    } else {
        SENTRY_DEBUGF("ptrace(PTRACE_GETREGSET) failed for thread %d: %s", tid,
            strerror(errno));
    }
#    elif defined(__i386__)
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
        // Map to ucontext_t format
        uctx->uc_mcontext.gregs[REG_GS] = regs.xgs;
        uctx->uc_mcontext.gregs[REG_FS] = regs.xfs;
        uctx->uc_mcontext.gregs[REG_ES] = regs.xes;
        uctx->uc_mcontext.gregs[REG_DS] = regs.xds;
        uctx->uc_mcontext.gregs[REG_EDI] = regs.edi;
        uctx->uc_mcontext.gregs[REG_ESI] = regs.esi;
        uctx->uc_mcontext.gregs[REG_EBP] = regs.ebp;
        uctx->uc_mcontext.gregs[REG_ESP] = regs.esp;
        uctx->uc_mcontext.gregs[REG_EBX] = regs.ebx;
        uctx->uc_mcontext.gregs[REG_EDX] = regs.edx;
        uctx->uc_mcontext.gregs[REG_ECX] = regs.ecx;
        uctx->uc_mcontext.gregs[REG_EAX] = regs.eax;
        uctx->uc_mcontext.gregs[REG_TRAPNO] = 0;
        uctx->uc_mcontext.gregs[REG_ERR] = 0;
        uctx->uc_mcontext.gregs[REG_EIP] = regs.eip;
        uctx->uc_mcontext.gregs[REG_CS] = regs.xcs;
        uctx->uc_mcontext.gregs[REG_EFL] = regs.eflags;
        uctx->uc_mcontext.gregs[REG_UESP] = regs.esp;
        uctx->uc_mcontext.gregs[REG_SS] = regs.xss;
        success = true;
        SENTRY_DEBUGF(
            "Thread %d: captured registers via ptrace, SP=0x%x", tid, regs.esp);
    } else {
        SENTRY_DEBUGF("ptrace(PTRACE_GETREGS) failed for thread %d: %s", tid,
            strerror(errno));
    }
#    elif defined(__arm__)
    struct user_regs regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
        // uregs[0..15] = R0..R15, uregs[16] = CPSR
        uctx->uc_mcontext.arm_r0 = regs.uregs[0];
        uctx->uc_mcontext.arm_r1 = regs.uregs[1];
        uctx->uc_mcontext.arm_r2 = regs.uregs[2];
        uctx->uc_mcontext.arm_r3 = regs.uregs[3];
        uctx->uc_mcontext.arm_r4 = regs.uregs[4];
        uctx->uc_mcontext.arm_r5 = regs.uregs[5];
        uctx->uc_mcontext.arm_r6 = regs.uregs[6];
        uctx->uc_mcontext.arm_r7 = regs.uregs[7];
        uctx->uc_mcontext.arm_r8 = regs.uregs[8];
        uctx->uc_mcontext.arm_r9 = regs.uregs[9];
        uctx->uc_mcontext.arm_r10 = regs.uregs[10];
        uctx->uc_mcontext.arm_fp = regs.uregs[11];
        uctx->uc_mcontext.arm_ip = regs.uregs[12];
        uctx->uc_mcontext.arm_sp = regs.uregs[13];
        uctx->uc_mcontext.arm_lr = regs.uregs[14];
        uctx->uc_mcontext.arm_pc = regs.uregs[15];
        uctx->uc_mcontext.arm_cpsr = regs.uregs[16];
        success = true;
        SENTRY_DEBUGF("Thread %d: captured registers via ptrace, SP=0x%lx", tid,
            (unsigned long)regs.uregs[13]);
    } else {
        SENTRY_DEBUGF("ptrace(PTRACE_GETREGS) failed for thread %d: %s", tid,
            strerror(errno));
    }
#    endif

    // Detach from thread
    ptrace(PTRACE_DETACH, tid, NULL, NULL);
    return success;
}

/**
 * Read memory from crashed process using process_vm_readv (fast bulk read)
 * Falls back to ptrace PEEKDATA if process_vm_readv fails
 */
static ssize_t
read_process_memory(
    minidump_writer_t *writer, uint64_t addr, void *buf, size_t len)
{
    if (!ptrace_attach_process(writer)) {
        return -1;
    }

    // Use crashed_tid for ptrace ops (we attached to this thread).
    // process_vm_readv works with any TID in the thread group.
    pid_t tid = writer->crash_ctx->crashed_tid;

    // Try process_vm_readv first - much faster for bulk reads
    // (single syscall vs one syscall per word with ptrace)
    struct iovec local_iov = { .iov_base = buf, .iov_len = len };
    struct iovec remote_iov = { .iov_base = (void *)addr, .iov_len = len };

    ssize_t nread = process_vm_readv(tid, &local_iov, 1, &remote_iov, 1, 0);
    if (nread > 0) {
        return nread;
    }

    // Fall back to ptrace word-by-word read if process_vm_readv fails
    // This is much slower but works in more restricted environments
    SENTRY_DEBUGF("process_vm_readv failed for tid %d at 0x%llx: %s, falling "
                  "back to ptrace",
        tid, (unsigned long long)addr, strerror(errno));

    size_t bytes_read = 0;
    uint8_t *byte_buf = (uint8_t *)buf;
    uint64_t current_addr = addr;

    while (bytes_read < len) {
        // Align to word boundary for ptrace
        uint64_t aligned_addr = current_addr & ~(sizeof(long) - 1);
        size_t offset_in_word = current_addr - aligned_addr;

        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, tid, aligned_addr, NULL);
        if (errno != 0) {
            if (bytes_read > 0) {
                // Return partial read
                return bytes_read;
            }
            SENTRY_DEBUGF("ptrace(PTRACE_PEEKDATA) failed at 0x%llx: %s",
                (unsigned long long)aligned_addr, strerror(errno));
            return -1;
        }

        // Copy relevant bytes from this word
        uint8_t *word_bytes = (uint8_t *)&word;
        size_t bytes_from_word
            = sizeof(long) - offset_in_word < len - bytes_read
            ? sizeof(long) - offset_in_word
            : len - bytes_read;

        memcpy(byte_buf + bytes_read, word_bytes + offset_in_word,
            bytes_from_word);

        bytes_read += bytes_from_word;
        current_addr += bytes_from_word;
    }

    return bytes_read;
}

/**
 * Parse /proc/[pid]/maps to get memory mappings
 */
static int
parse_proc_maps(minidump_writer_t *writer)
{
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps",
        writer->crash_ctx->crashed_pid);

    FILE *f = fopen(maps_path, "r");
    if (!f) {
        SENTRY_WARNF("failed to open %s: %s", maps_path, strerror(errno));
        return -1;
    }

    char line[1024];
    writer->mapping_count = 0;

    while (fgets(line, sizeof(line), f)
        && writer->mapping_count < SENTRY_CRASH_MAX_MAPPINGS) {
        memory_mapping_t *mapping = &writer->mappings[writer->mapping_count];

        // Parse line: "start-end perms offset dev inode pathname"
        unsigned long long start, end, offset;
        char perms[5];
        int pathname_offset = 0;

        int parsed = sscanf(line, "%llx-%llx %4s %llx %*s %*s %n", &start, &end,
            perms, &offset, &pathname_offset);

        if (parsed >= 4) {
            mapping->start = start;
            mapping->end = end;
            mapping->offset = offset;
            memcpy(mapping->permissions, perms, 4);
            mapping->permissions[4] = '\0';

            // Extract pathname if present
            if (pathname_offset > 0 && line[pathname_offset] != '\0') {
                const char *pathname = line + pathname_offset;
                // Trim newline
                size_t len = strlen(pathname);
                if (len > 0 && pathname[len - 1] == '\n') {
                    len--;
                }
                size_t copy_len = len < sizeof(mapping->name) - 1
                    ? len
                    : sizeof(mapping->name) - 1;
                memcpy(mapping->name, pathname, copy_len);
                mapping->name[copy_len] = '\0';
            } else {
                mapping->name[0] = '\0';
            }

            writer->mapping_count++;
        }
    }

    fclose(f);

    SENTRY_DEBUGF("parsed %zu memory mappings", writer->mapping_count);
    return 0;
}

/**
 * Enumerate threads from /proc/[pid]/task
 */
static int
enumerate_threads(minidump_writer_t *writer)
{
    char task_path[64];
    snprintf(task_path, sizeof(task_path), "/proc/%d/task",
        writer->crash_ctx->crashed_pid);

    DIR *dir = opendir(task_path);
    if (!dir) {
        SENTRY_WARNF("failed to open %s: %s", task_path, strerror(errno));
        return -1;
    }

    writer->thread_count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir))
        && writer->thread_count < SENTRY_CRASH_MAX_THREADS) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        pid_t tid = (pid_t)atoi(entry->d_name);
        if (tid > 0) {
            size_t idx = writer->thread_count;
            writer->tids[idx] = tid;

            // Read thread name from /proc/[pid]/task/[tid]/comm
            char comm_path[64];
            snprintf(comm_path, sizeof(comm_path), "/proc/%d/task/%d/comm",
                writer->crash_ctx->crashed_pid, tid);
            FILE *comm_file = fopen(comm_path, "r");
            if (comm_file) {
                if (fgets(writer->thread_names[idx],
                        sizeof(writer->thread_names[idx]), comm_file)) {
                    // Trim trailing newline
                    size_t len = strlen(writer->thread_names[idx]);
                    if (len > 0 && writer->thread_names[idx][len - 1] == '\n') {
                        writer->thread_names[idx][len - 1] = '\0';
                    }
                } else {
                    writer->thread_names[idx][0] = '\0';
                }
                fclose(comm_file);
            } else {
                writer->thread_names[idx][0] = '\0';
            }

            writer->thread_count++;
        }
    }

    closedir(dir);

    SENTRY_DEBUGF("found %zu threads", writer->thread_count);
    return 0;
}

// Use common minidump functions (cast writer to base type)
#    define write_data(writer, data, size)                                     \
        sentry__minidump_write_data(                                           \
            (minidump_writer_base_t *)(writer), (data), (size))
#    define write_header(writer, stream_count)                                 \
        sentry__minidump_write_header(                                         \
            (minidump_writer_base_t *)(writer), (stream_count))
#    define write_minidump_string(writer, str)                                 \
        sentry__minidump_write_string((minidump_writer_base_t *)(writer), (str))
#    define get_context_size() sentry__minidump_get_context_size()

/**
 * Write system info stream
 */
static int
write_system_info_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    minidump_system_info_t sysinfo = { 0 };

#    if defined(__x86_64__)
    sysinfo.processor_architecture = MINIDUMP_CPU_X86_64;
#    elif defined(__aarch64__)
    sysinfo.processor_architecture = MINIDUMP_CPU_ARM64;
#    elif defined(__i386__)
    sysinfo.processor_architecture = MINIDUMP_CPU_X86;
#    elif defined(__arm__)
    sysinfo.processor_architecture = MINIDUMP_CPU_ARM;
#    endif

#    if defined(SENTRY_PLATFORM_ANDROID)
    sysinfo.platform_id = MINIDUMP_OS_ANDROID;
#    else
    sysinfo.platform_id = MINIDUMP_OS_LINUX;
#    endif

    sysinfo.number_of_processors = (uint8_t)sysconf(_SC_NPROCESSORS_ONLN);

    // Populate OS version from uname(), matching Crashpad behavior
    struct utsname uts;
    char csd_version[512] = "";
    bool have_uname = false;
    if (uname(&uts) == 0) {
        have_uname = true;
        int major = 0, minor = 0, patch = 0;
        sscanf(uts.release, "%d.%d.%d", &major, &minor, &patch);
        sysinfo.major_version = (uint32_t)major;
        sysinfo.minor_version = (uint32_t)minor;
        sysinfo.build_number = (uint32_t)patch;

        snprintf(csd_version, sizeof(csd_version), "%s %s %s %s", uts.sysname,
            uts.release, uts.version, uts.machine);
    } else {
        SENTRY_WARNF("uname() failed: %s — falling back to "
                     "/proc/sys/kernel/osrelease",
            strerror(errno));
    }

    // Fallback when uname succeeds but release didn't parse, or uname is
    // blocked entirely (sandboxed/seccomp environments often return "(none)"
    // or disable the syscall).
    if (sysinfo.major_version == 0 && sysinfo.minor_version == 0
        && sysinfo.build_number == 0) {
        int rfd = open("/proc/sys/kernel/osrelease", O_RDONLY);
        if (rfd >= 0) {
            char buf[64] = { 0 };
            ssize_t n = read(rfd, buf, sizeof(buf) - 1);
            close(rfd);
            if (n > 0) {
                int major = 0, minor = 0, patch = 0;
                if (sscanf(buf, "%d.%d.%d", &major, &minor, &patch) >= 1) {
                    sysinfo.major_version = (uint32_t)major;
                    sysinfo.minor_version = (uint32_t)minor;
                    sysinfo.build_number = (uint32_t)patch;
                }
            }
        }
        if (sysinfo.major_version == 0 && sysinfo.minor_version == 0
            && sysinfo.build_number == 0) {
            SENTRY_WARNF(
                "OS version unavailable (uname %s, /proc fallback failed)",
                have_uname ? "release unparseable" : "blocked");
        }
    }
    sysinfo.csd_version_rva = write_minidump_string(writer, csd_version);
    if (!sysinfo.csd_version_rva) {
        return -1;
    }

    dir->stream_type = MINIDUMP_STREAM_SYSTEM_INFO;
    dir->rva = write_data(writer, &sysinfo, sizeof(sysinfo));
    dir->data_size = sizeof(sysinfo);

    return dir->rva ? 0 : -1;
}

#    if defined(__aarch64__)
/**
 * Parse the __reserved field in mcontext to find FPSIMD context
 */
static const struct fpsimd_context *
find_fpsimd_context(const ucontext_t *uctx)
{
    // The __reserved field contains a chain of context blocks
    const uint8_t *ptr = (const uint8_t *)uctx->uc_mcontext.__reserved;
    const uint8_t *end = ptr + sizeof(uctx->uc_mcontext.__reserved);

    // Walk through context blocks looking for FPSIMD_MAGIC
    while (ptr + sizeof(struct _aarch64_ctx) <= end) {
        const struct _aarch64_ctx *ctx = (const struct _aarch64_ctx *)ptr;

        // Check for end marker (magic = 0, size = 0)
        if (ctx->magic == 0 && ctx->size == 0) {
            break;
        }

        // Check for valid size
        if (ctx->size == 0 || ctx->size > (size_t)(end - ptr)) {
            break;
        }

        // Found FPSIMD context
        if (ctx->magic == FPSIMD_MAGIC) {
            if (ctx->size >= sizeof(struct fpsimd_context)) {
                return (const struct fpsimd_context *)ctx;
            }
            break;
        }

        // Move to next context block
        ptr += ctx->size;
    }

    return NULL;
}
#    endif

/**
 * Convert Linux ucontext_t to minidump context
 */
static minidump_rva_t
write_thread_context(
    minidump_writer_t *writer, const ucontext_t *uctx, pid_t tid)
{
    if (!uctx) {
        return 0;
    }

#    if defined(__x86_64__)
    minidump_context_x86_64_t context = { 0 };
    // Set flags for full context (control + integer + segments + floating
    // point)
    context.context_flags
        = 0x0010003f; // CONTEXT_AMD64 | CONTEXT_CONTROL | CONTEXT_INTEGER |
                      // CONTEXT_SEGMENTS | CONTEXT_FLOATING_POINT

    // Copy general purpose registers from Linux ucontext
    context.rax = uctx->uc_mcontext.gregs[REG_RAX];
    context.rbx = uctx->uc_mcontext.gregs[REG_RBX];
    context.rcx = uctx->uc_mcontext.gregs[REG_RCX];
    context.rdx = uctx->uc_mcontext.gregs[REG_RDX];
    context.rsi = uctx->uc_mcontext.gregs[REG_RSI];
    context.rdi = uctx->uc_mcontext.gregs[REG_RDI];
    context.rbp = uctx->uc_mcontext.gregs[REG_RBP];
    context.rsp = uctx->uc_mcontext.gregs[REG_RSP];
    context.r8 = uctx->uc_mcontext.gregs[REG_R8];
    context.r9 = uctx->uc_mcontext.gregs[REG_R9];
    context.r10 = uctx->uc_mcontext.gregs[REG_R10];
    context.r11 = uctx->uc_mcontext.gregs[REG_R11];
    context.r12 = uctx->uc_mcontext.gregs[REG_R12];
    context.r13 = uctx->uc_mcontext.gregs[REG_R13];
    context.r14 = uctx->uc_mcontext.gregs[REG_R14];
    context.r15 = uctx->uc_mcontext.gregs[REG_R15];
    context.rip = uctx->uc_mcontext.gregs[REG_RIP];
    context.eflags = uctx->uc_mcontext.gregs[REG_EFL];
    context.cs = uctx->uc_mcontext.gregs[REG_CSGSFS] & 0xffff;

    // Try to capture FPU state via ptrace for crashed thread
    // The fpregs pointer from ucontext is invalid in daemon process
    if (tid == writer->crash_ctx->crashed_tid && writer->ptrace_attached) {
        struct user_fpregs_struct fpregs;
        if (ptrace_get_fpregs(tid, &fpregs)) {
            SENTRY_DEBUGF("Thread %d: copying FPU registers to context", tid);

            // Copy x87 FPU registers (ST0-ST7)
            // Each ST register is 10 bytes (80-bit), but stored in 16-byte
            // m128a_t Linux st_space is uint32_t[32], with each register
            // occupying 4 uint32_t (16 bytes)
            for (int i = 0; i < 8; i++) {
                // Copy 10 bytes of actual FPU data, leave upper 6 bytes as zero
                memcpy(&context.float_save.float_registers[i],
                    &fpregs.st_space[i * 4], 10);
            }
            SENTRY_DEBUGF("Thread %d: copied x87 registers", tid);

            // Copy control/status words
            context.float_save.control_word = fpregs.cwd;
            context.float_save.status_word = fpregs.swd;
            context.float_save.tag_word = fpregs.ftw;
            context.float_save.error_opcode = fpregs.fop;
            // On x86_64, FPU IP/DP are 64-bit. The FXSAVE format splits them
            // across offset (low 32) and selector (high 16) fields.
            context.float_save.error_offset = (uint32_t)fpregs.rip;
            context.float_save.error_selector = (uint16_t)(fpregs.rip >> 32);
            context.float_save.data_offset = (uint32_t)fpregs.rdp;
            context.float_save.data_selector = (uint16_t)(fpregs.rdp >> 32);
            SENTRY_DEBUGF("Thread %d: copied control/status words", tid);

            // Copy XMM registers (XMM0-XMM15)
            memcpy(context.float_save.xmm_registers, fpregs.xmm_space,
                sizeof(context.float_save.xmm_registers));
            context.float_save.mx_csr = fpregs.mxcsr;
            SENTRY_DEBUGF("Thread %d: copied XMM registers", tid);
        }
    }

    SENTRY_DEBUGF("Thread %d: about to write context data", tid);
    minidump_rva_t rva = write_data(writer, &context, sizeof(context));
    SENTRY_DEBUGF("Thread %d: wrote context at RVA 0x%x", tid, rva);
    return rva;

#    elif defined(__aarch64__)
    (void)tid; // Unused on ARM64 - FPU state already in ucontext

    minidump_context_arm64_t context = { 0 };
    // Set flags for control + integer + fpsimd registers (FULL context)
    context.context_flags = 0x00400007; // ARM64 | Control | Integer | Fpsimd

    // Copy general purpose registers X0-X28
    for (int i = 0; i < 29; i++) {
        context.regs[i] = uctx->uc_mcontext.regs[i];
    }
    // Copy FP, LR, SP, PC separately
    context.fp = uctx->uc_mcontext.regs[29]; // X29
    context.lr = uctx->uc_mcontext.regs[30]; // X30
    context.sp = uctx->uc_mcontext.sp;
    context.pc = uctx->uc_mcontext.pc;
    context.cpsr = uctx->uc_mcontext.pstate;

    // Parse __reserved field to find FPSIMD context with NEON/FP registers
    const struct fpsimd_context *fpsimd = find_fpsimd_context(uctx);
    if (fpsimd) {
        // Copy NEON/FP registers V0-V31 from Linux __uint128_t to our
        // uint128_struct
        for (int i = 0; i < 32; i++) {
            __uint128_t vreg = fpsimd->vregs[i];
            context.fpsimd[i].low = (uint64_t)vreg;
            context.fpsimd[i].high = (uint64_t)(vreg >> 64);
        }
        context.fpsr = fpsimd->fpsr;
        context.fpcr = fpsimd->fpcr;
    } else {
        // FPSIMD context not found, zero out registers
        memset(context.fpsimd, 0, sizeof(context.fpsimd));
        context.fpsr = 0;
        context.fpcr = 0;
    }

    // Zero out debug registers
    memset(context.bcr, 0, sizeof(context.bcr));
    memset(context.bvr, 0, sizeof(context.bvr));
    memset(context.wcr, 0, sizeof(context.wcr));
    memset(context.wvr, 0, sizeof(context.wvr));

    return write_data(writer, &context, sizeof(context));

#    elif defined(__i386__)
    (void)tid; // Unused on i386 - no ptrace FPU capture implemented yet

    minidump_context_x86_t context = { 0 };
    // Set flags for full context (control + integer + segments + floating
    // point)
    context.context_flags = 0x0001001f; // CONTEXT_i386 | CONTEXT_CONTROL |
                                        // CONTEXT_INTEGER | CONTEXT_SEGMENTS

    // Copy general purpose registers from Linux ucontext
    context.eax = uctx->uc_mcontext.gregs[REG_EAX];
    context.ebx = uctx->uc_mcontext.gregs[REG_EBX];
    context.ecx = uctx->uc_mcontext.gregs[REG_ECX];
    context.edx = uctx->uc_mcontext.gregs[REG_EDX];
    context.esi = uctx->uc_mcontext.gregs[REG_ESI];
    context.edi = uctx->uc_mcontext.gregs[REG_EDI];
    context.ebp = uctx->uc_mcontext.gregs[REG_EBP];
    context.esp = uctx->uc_mcontext.gregs[REG_ESP];
    context.eip = uctx->uc_mcontext.gregs[REG_EIP];
    context.eflags = uctx->uc_mcontext.gregs[REG_EFL];
    context.cs = uctx->uc_mcontext.gregs[REG_CS];
    context.ds = uctx->uc_mcontext.gregs[REG_DS];
    context.es = uctx->uc_mcontext.gregs[REG_ES];
    context.fs = uctx->uc_mcontext.gregs[REG_FS];
    context.gs = uctx->uc_mcontext.gregs[REG_GS];
    context.ss = uctx->uc_mcontext.gregs[REG_SS];

    // Debug registers - zero out (not available from ucontext)
    context.dr0 = 0;
    context.dr1 = 0;
    context.dr2 = 0;
    context.dr3 = 0;
    context.dr6 = 0;
    context.dr7 = 0;

    // FPU state - zero out (could be captured via ptrace GETFPREGS in future)
    memset(&context.float_save, 0, sizeof(context.float_save));
    memset(context.extended_registers, 0, sizeof(context.extended_registers));

    return write_data(writer, &context, sizeof(context));

#    elif defined(__arm__)
    (void)tid; // Unused on ARM32 - no VFP capture implemented yet

    minidump_context_arm_t context = { 0 };
    // MD_CONTEXT_ARM | CONTROL | INTEGER — breakpad-style 0x40000000 base
    // (not Microsoft's 0x00200000; rust-minidump keys off the breakpad value)
    context.context_flags = 0x40000003;

    // Copy general purpose registers R0-R10 from Linux ucontext
    context.r[0] = uctx->uc_mcontext.arm_r0;
    context.r[1] = uctx->uc_mcontext.arm_r1;
    context.r[2] = uctx->uc_mcontext.arm_r2;
    context.r[3] = uctx->uc_mcontext.arm_r3;
    context.r[4] = uctx->uc_mcontext.arm_r4;
    context.r[5] = uctx->uc_mcontext.arm_r5;
    context.r[6] = uctx->uc_mcontext.arm_r6;
    context.r[7] = uctx->uc_mcontext.arm_r7;
    context.r[8] = uctx->uc_mcontext.arm_r8;
    context.r[9] = uctx->uc_mcontext.arm_r9;
    context.r[10] = uctx->uc_mcontext.arm_r10;
    context.r[11] = uctx->uc_mcontext.arm_fp; // R11 (FP)
    context.r[12] = uctx->uc_mcontext.arm_ip; // R12 (IP)
    context.sp = uctx->uc_mcontext.arm_sp;
    context.lr = uctx->uc_mcontext.arm_lr;
    context.pc = uctx->uc_mcontext.arm_pc;
    context.cpsr = uctx->uc_mcontext.arm_cpsr;

    return write_data(writer, &context, sizeof(context));

#    else
#        error "Unsupported architecture for Linux"
#    endif
}

/**
 * Quickly verify a file is an ELF binary by reading its magic bytes.
 * Used to filter out non-ELF mappings (e.g. files under /dev/shm, deleted
 * files, sentry-native's own IPC shared memory) from the module list — LLDB and
 * other consumers can choke on entries that look like modules but aren't.
 */
static bool
is_elf_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    unsigned char magic[SELFMAG];
    bool is_elf = read(fd, magic, SELFMAG) == (ssize_t)SELFMAG
        && memcmp(magic, ELFMAG, SELFMAG) == 0;
    close(fd);
    return is_elf;
}

/**
 * Extract Build ID from ELF file
 * Returns the Build ID length, or 0 if not found
 */
static size_t
extract_elf_build_id(const char *elf_path, uint8_t *build_id, size_t max_len)
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
    // Cast to size_t to prevent integer overflow (uint16_t * uint16_t promotes
    // to int, which can overflow)
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
        if (sections[i].sh_type == SHT_NOTE) {
            // Read note section
            size_t note_size = sections[i].sh_size;
            if (note_size > 4096)
                continue; // Sanity check

            void *note_buf = sentry_malloc(note_size);
            if (!note_buf)
                continue;

            if (lseek(fd, sections[i].sh_offset, SEEK_SET)
                    == (off_t)sections[i].sh_offset
                && read(fd, note_buf, note_size) == (ssize_t)note_size) {

                // Parse notes
                uint8_t *ptr = (uint8_t *)note_buf;
                uint8_t *end = ptr + note_size;

                while (ptr + 12 <= end) {
#    if defined(__x86_64__) || defined(__aarch64__)
                    Elf64_Nhdr *nhdr = (Elf64_Nhdr *)ptr;
#    else
                    Elf32_Nhdr *nhdr = (Elf32_Nhdr *)ptr;
#    endif
                    ptr += sizeof(*nhdr);

                    // Use aligned sizes in bounds check since pointer advances
                    // by aligned amounts. Also check for zero advancement to
                    // prevent infinite loop on malformed notes (e.g., overflow
                    // on 32-bit when n_namesz/n_descsz are near UINT32_MAX)
                    size_t aligned_namesz = ((nhdr->n_namesz + 3) & ~3);
                    size_t aligned_descsz = ((nhdr->n_descsz + 3) & ~3);
                    if (aligned_namesz == 0 && aligned_descsz == 0)
                        break; // Prevent infinite loop
                    if (ptr + aligned_namesz + aligned_descsz > end)
                        break;

                    // Check if this is GNU Build ID (type 3, name "GNU\0")
                    if (nhdr->n_type == 3 && nhdr->n_namesz == 4
                        && memcmp(ptr, "GNU", 4) == 0) {

                        ptr += aligned_namesz;
                        size_t len = nhdr->n_descsz < max_len ? nhdr->n_descsz
                                                              : max_len;
                        memcpy(build_id, ptr, len);
                        build_id_len = len;
                        sentry_free(note_buf);
                        goto done;
                    }

                    ptr += aligned_namesz;
                    ptr += aligned_descsz;
                }
            }

            sentry_free(note_buf);
        }
    }

done:
    sentry_free(shdr_buf);
    close(fd);
    return build_id_len;
}

/**
 * Compute the virtual memory size of an ELF file from its PT_LOAD segments.
 * This matches Crashpad's behavior: size = max(p_vaddr + p_memsz) -
 * min(p_vaddr) across all PT_LOAD segments, rather than using page-aligned
 * /proc/maps sizes.
 * Returns the computed size, or 0 on failure (caller should fall back to
 * /proc/maps size).
 */
static uint64_t
compute_elf_size_from_phdrs(const char *elf_path)
{
    int fd = open(elf_path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    Elf64_Ehdr ehdr;
#    else
    Elf32_Ehdr ehdr;
#    endif

    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)
        || memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_phnum == 0) {
        close(fd);
        return 0;
    }

    if (!sentry__elf_has_phdr_size(ehdr.e_ident, ehdr.e_phentsize)) {
        close(fd);
        return 0;
    }

    // Read program headers
    size_t phdr_size = (size_t)ehdr.e_phentsize * ehdr.e_phnum;
    void *phdr_buf = sentry_malloc(phdr_size);
    if (!phdr_buf) {
        close(fd);
        return 0;
    }

    if (lseek(fd, ehdr.e_phoff, SEEK_SET) != (off_t)ehdr.e_phoff
        || read(fd, phdr_buf, phdr_size) != (ssize_t)phdr_size) {
        sentry_free(phdr_buf);
        close(fd);
        return 0;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    Elf64_Phdr *phdrs = (Elf64_Phdr *)phdr_buf;
#    else
    Elf32_Phdr *phdrs = (Elf32_Phdr *)phdr_buf;
#    endif

    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    bool found_load = false;

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            found_load = true;
            if (phdrs[i].p_vaddr < min_vaddr) {
                min_vaddr = phdrs[i].p_vaddr;
            }
            uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
            if (end > max_vaddr) {
                max_vaddr = end;
            }
        }
    }

    sentry_free(phdr_buf);
    close(fd);

    if (!found_load || max_vaddr <= min_vaddr) {
        return 0;
    }

    return max_vaddr - min_vaddr;
}

/**
 * Read the DT_SONAME from an ELF file's dynamic section.
 * Crashpad uses SONAME as the module name, falling back to the /proc/maps
 * path. This function reads the .dynamic section to find DT_SONAME and
 * resolves it through .dynstr.
 * Returns true if SONAME was found and written to soname_buf.
 */
static bool
read_elf_soname(const char *elf_path, char *soname_buf, size_t soname_buf_size)
{
    int fd = open(elf_path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

#    if defined(__x86_64__) || defined(__aarch64__)
    Elf64_Ehdr ehdr;
    typedef Elf64_Dyn DynT;
    typedef Elf64_Shdr ShdrT;
#    else
    Elf32_Ehdr ehdr;
    typedef Elf32_Dyn DynT;
    typedef Elf32_Shdr ShdrT;
#    endif

    bool result = false;

    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)
        || memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        close(fd);
        return false;
    }

    if (!sentry__elf_has_shdr_size(ehdr.e_ident, ehdr.e_shentsize)) {
        close(fd);
        return false;
    }

    // Read section headers to find .dynamic and .dynstr
    size_t shdr_size = (size_t)ehdr.e_shentsize * ehdr.e_shnum;
    void *shdr_buf = sentry_malloc(shdr_size);
    if (!shdr_buf) {
        close(fd);
        return false;
    }

    if (lseek(fd, ehdr.e_shoff, SEEK_SET) != (off_t)ehdr.e_shoff
        || read(fd, shdr_buf, shdr_size) != (ssize_t)shdr_size) {
        sentry_free(shdr_buf);
        close(fd);
        return false;
    }

    ShdrT *sections = (ShdrT *)shdr_buf;

    // Find .dynamic and .dynstr sections
    ShdrT *dynamic_shdr = NULL;
    ShdrT *dynstr_shdr = NULL;
    for (int i = 0; i < ehdr.e_shnum; i++) {
        if (sections[i].sh_type == SHT_DYNAMIC) {
            dynamic_shdr = &sections[i];
        } else if (sections[i].sh_type == SHT_STRTAB && !dynstr_shdr) {
            // The .dynstr is typically the first SHT_STRTAB that is also
            // referenced by a SHT_DYNAMIC section's sh_link
            dynstr_shdr = &sections[i];
        }
    }

    // Use sh_link from .dynamic to find the correct string table
    if (dynamic_shdr && dynamic_shdr->sh_link < ehdr.e_shnum) {
        dynstr_shdr = &sections[dynamic_shdr->sh_link];
    } else {
        // Discard the SHT_STRTAB fallback — it may be .shstrtab, not .dynstr
        dynstr_shdr = NULL;
    }

    if (!dynamic_shdr || !dynstr_shdr) {
        sentry_free(shdr_buf);
        close(fd);
        return false;
    }

    // Read .dynstr
    size_t dynstr_size = dynstr_shdr->sh_size;
    if (dynstr_size > 1024 * 1024) { // Sanity: max 1MB
        sentry_free(shdr_buf);
        close(fd);
        return false;
    }
    char *dynstr = sentry_malloc(dynstr_size);
    if (!dynstr) {
        sentry_free(shdr_buf);
        close(fd);
        return false;
    }
    if (lseek(fd, dynstr_shdr->sh_offset, SEEK_SET)
            != (off_t)dynstr_shdr->sh_offset
        || read(fd, dynstr, dynstr_size) != (ssize_t)dynstr_size) {
        sentry_free(dynstr);
        sentry_free(shdr_buf);
        close(fd);
        return false;
    }

    // Read .dynamic entries and find DT_SONAME
    size_t dyn_size = dynamic_shdr->sh_size;
    void *dyn_buf = sentry_malloc(dyn_size);
    if (!dyn_buf) {
        sentry_free(dynstr);
        sentry_free(shdr_buf);
        close(fd);
        return false;
    }
    if (lseek(fd, dynamic_shdr->sh_offset, SEEK_SET)
            != (off_t)dynamic_shdr->sh_offset
        || read(fd, dyn_buf, dyn_size) != (ssize_t)dyn_size) {
        sentry_free(dyn_buf);
        sentry_free(dynstr);
        sentry_free(shdr_buf);
        close(fd);
        return false;
    }

    DynT *dyn_entries = (DynT *)dyn_buf;
    size_t dyn_count = dyn_size / sizeof(DynT);

    for (size_t i = 0; i < dyn_count; i++) {
        if (dyn_entries[i].d_tag == DT_SONAME) {
            size_t name_offset = dyn_entries[i].d_un.d_val;
            if (name_offset < dynstr_size) {
                const char *soname = dynstr + name_offset;
                size_t len = strnlen(soname, dynstr_size - name_offset);
                if (len > 0 && len < soname_buf_size) {
                    memcpy(soname_buf, soname, len);
                    soname_buf[len] = '\0';
                    result = true;
                }
            }
            break;
        }
        if (dyn_entries[i].d_tag == DT_NULL) {
            break;
        }
    }

    sentry_free(dyn_buf);
    sentry_free(dynstr);
    sentry_free(shdr_buf);
    close(fd);
    return result;
}

/**
 * Write CodeView record with Build ID
 */
static minidump_rva_t
write_cv_record(minidump_writer_t *writer, const char *module_path,
    const uint8_t *build_id, size_t build_id_len)
{
    (void)module_path; // Not used in ELF format (only signature + build_id)

    if (!build_id || build_id_len == 0) {
        return 0;
    }

    // Calculate size: signature (4 bytes) + build_id (variable length)
    // Note: Breakpad's format is just signature + raw build_id bytes
    // No filename is stored in the CV record for ELF
    size_t total_size = sizeof(uint32_t) + build_id_len;

    uint8_t *cv_record = sentry_malloc(total_size);
    if (!cv_record) {
        return 0;
    }

    // Write 'BpEL' signature (0x4270454c)
    uint32_t signature = CV_SIGNATURE_ELF;
    memcpy(cv_record, &signature, sizeof(signature));

    // Write raw Build ID bytes (typically 20 bytes for SHA-1)
    memcpy(cv_record + sizeof(signature), build_id, build_id_len);

    SENTRY_DEBUGF(
        "CV Record: signature=0x%x, build_id_len=%zu", signature, build_id_len);

    minidump_rva_t rva = write_data(writer, cv_record, total_size);
    sentry_free(cv_record);
    return rva;
}

/**
 * Write stack memory for a thread
 * Returns RVA to stack data, and sets stack_size_out and stack_start_out
 */
static minidump_rva_t
write_thread_stack(minidump_writer_t *writer, uint64_t stack_pointer,
    size_t *stack_size_out, uint64_t *stack_start_out)
{
    SENTRY_DEBUGF(
        "write_thread_stack: SP=0x%llx", (unsigned long long)stack_pointer);

    // On x86_64, include the red zone (128 bytes below SP)
    // Leaf functions can use this area without adjusting SP
#    if defined(__x86_64__)
    const size_t RED_ZONE = 128;
    uint64_t capture_start
        = stack_pointer >= RED_ZONE ? stack_pointer - RED_ZONE : stack_pointer;
#    else
    uint64_t capture_start = stack_pointer;
#    endif

    // Find the stack mapping for this thread.
    // First try named [stack] mappings (main thread), then fall back to any
    // anonymous rw-p mapping containing the SP (non-main thread stacks are
    // anonymous on Linux).
    uint64_t stack_start = 0;
    uint64_t stack_end = 0;

    for (size_t i = 0; i < writer->mapping_count; i++) {
        if (stack_pointer >= writer->mappings[i].start
            && stack_pointer < writer->mappings[i].end
            && strstr(writer->mappings[i].name, "[stack") != NULL) {
            stack_start = writer->mappings[i].start;
            stack_end = writer->mappings[i].end;
            break;
        }
    }

    if (stack_start == 0) {
        // No named [stack] mapping found. Look for an anonymous rw-p mapping
        // containing the SP (this is how non-main thread stacks appear in
        // /proc/pid/maps).
        for (size_t i = 0; i < writer->mapping_count; i++) {
            if (stack_pointer >= writer->mappings[i].start
                && stack_pointer < writer->mappings[i].end
                && writer->mappings[i].permissions[0] == 'r'
                && writer->mappings[i].permissions[1] == 'w'
                && writer->mappings[i].permissions[2] != 'x'
                && writer->mappings[i].name[0] == '\0') {
                stack_start = writer->mappings[i].start;
                stack_end = writer->mappings[i].end;
                break;
            }
        }
    }

    if (stack_start == 0) {
        // Stack mapping not found, use a reasonable range
        const size_t DEFAULT_STACK_SIZE = SENTRY_CRASH_MAX_STACK_CAPTURE;
        stack_start = capture_start;
        stack_end = stack_pointer + DEFAULT_STACK_SIZE;
    }

    // Ensure capture_start is within stack bounds
    if (capture_start < stack_start) {
        capture_start = stack_start;
    }

    // Capture from adjusted SP to end of stack (upwards)
    size_t stack_size = stack_end - capture_start;

    // Limit to 1MB
    if (stack_size > SENTRY_CRASH_MAX_STACK_SIZE) {
        stack_size = SENTRY_CRASH_MAX_STACK_SIZE;
    }

    void *stack_buffer = sentry_malloc(stack_size);
    if (!stack_buffer) {
        *stack_size_out = 0;
        *stack_start_out = 0;
        return 0;
    }

    // Read stack memory from crashed process (including red zone if applicable)
    ssize_t nread
        = read_process_memory(writer, capture_start, stack_buffer, stack_size);

    minidump_rva_t rva = 0;
    if (nread > 0) {
        rva = write_data(writer, stack_buffer, nread);
        // Only set size/start if write succeeded; rva=0 with size>0 would
        // cause parsers to read stack data from offset 0 (the minidump header).
        *stack_size_out = rva ? (size_t)nread : 0;
        *stack_start_out = rva ? capture_start : 0;
        SENTRY_DEBUGF(
            "Read %zd bytes of stack memory from 0x%llx (SP was 0x%llx)", nread,
            (unsigned long long)capture_start,
            (unsigned long long)stack_pointer);
    } else {
        SENTRY_WARNF(
            "Failed to read stack memory from process %d at 0x%llx (size %zu): "
            "%s",
            writer->crash_ctx->crashed_pid, (unsigned long long)capture_start,
            stack_size, strerror(errno));
        *stack_size_out = 0;
        *stack_start_out = 0;
    }

    sentry_free(stack_buffer);
    return rva;
}

/**
 * Push a thread's stack descriptor onto writer->thread_stacks so the
 * MemoryListStream writer can replay it. We only record stacks whose
 * write actually produced bytes — a zero-size or zero-rva descriptor
 * would point into the minidump header and break parsers.
 */
static void
record_thread_stack(minidump_writer_t *writer, const minidump_thread_t *thread)
{
    if (thread->stack.memory.size == 0 || thread->stack.memory.rva == 0) {
        return;
    }
    if (writer->thread_stack_count >= SENTRY_CRASH_MAX_THREADS) {
        return;
    }
    writer->thread_stacks[writer->thread_stack_count] = thread->stack;
    writer->thread_stack_tids[writer->thread_stack_count]
        = (pid_t)thread->thread_id;
    writer->thread_stack_count++;
}

/**
 * Capture a thread's context and stack via ptrace.
 * Writes the thread context and stack memory into the minidump and updates
 * the thread entry accordingly. Returns true on success.
 */
static bool
ptrace_capture_thread(
    minidump_writer_t *writer, minidump_thread_t *thread, const char *reason)
{
    SENTRY_DEBUGF(
        "Thread %u: %s, attempting ptrace capture", thread->thread_id, reason);

    ucontext_t ptrace_ctx;
    memset(&ptrace_ctx, 0, sizeof(ptrace_ctx));

    if (!ptrace_get_thread_registers(thread->thread_id, &ptrace_ctx)) {
        SENTRY_WARNF("Thread %u: ptrace capture failed, thread will have "
                     "no context or stack in minidump",
            thread->thread_id);
        return false;
    }

    SENTRY_DEBUGF(
        "Thread %u: successfully captured via ptrace", thread->thread_id);

    thread->thread_context.rva
        = write_thread_context(writer, &ptrace_ctx, thread->thread_id);
    thread->thread_context.size
        = thread->thread_context.rva ? get_context_size() : 0;

    uint64_t ptrace_sp;
#    if defined(__x86_64__)
    ptrace_sp = ptrace_ctx.uc_mcontext.gregs[REG_RSP];
#    elif defined(__aarch64__)
    ptrace_sp = ptrace_ctx.uc_mcontext.sp;
#    elif defined(__i386__)
    ptrace_sp = ptrace_ctx.uc_mcontext.gregs[REG_ESP];
#    elif defined(__arm__)
    ptrace_sp = ptrace_ctx.uc_mcontext.arm_sp;
#    endif

    if (ptrace_sp != 0) {
        size_t stack_size = 0;
        uint64_t stack_start = 0;
        thread->stack.memory.rva
            = write_thread_stack(writer, ptrace_sp, &stack_size, &stack_start);
        thread->stack.memory.size = stack_size;
        thread->stack.start_address = stack_start;
        record_thread_stack(writer, thread);

        SENTRY_DEBUGF("Thread %u: wrote ptrace context at RVA "
                      "0x%x, stack at RVA 0x%x (size %zu)",
            thread->thread_id, thread->thread_context.rva,
            thread->stack.memory.rva, stack_size);
    }

    return true;
}

/**
 * Write thread list stream
 */
static int
write_thread_list_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    SENTRY_DEBUGF(
        "write_thread_list_stream: %zu threads", writer->thread_count);

    // Calculate total size needed
    size_t list_size
        = sizeof(uint32_t) + (writer->thread_count * sizeof(minidump_thread_t));

    minidump_thread_list_t *thread_list = sentry_malloc(list_size);
    if (!thread_list) {
        SENTRY_WARN("Failed to allocate thread list");
        return -1;
    }

    thread_list->count = writer->thread_count;

    // Fill in thread info with context and stack
    for (size_t i = 0; i < writer->thread_count; i++) {
        SENTRY_DEBUGF("Processing thread %zu/%zu (tid=%d)", i + 1,
            writer->thread_count, writer->tids[i]);

        minidump_thread_t *thread = &thread_list->threads[i];
        memset(thread, 0, sizeof(*thread));

        thread->thread_id = writer->tids[i];

        // Try to find this thread in the captured threads
        const ucontext_t *uctx = NULL;
        size_t num_threads = writer->crash_ctx->platform.num_threads;
        // Bounds check to prevent out-of-bounds access on corrupted crash
        // context
        if (num_threads > SENTRY_CRASH_MAX_THREADS) {
            num_threads = SENTRY_CRASH_MAX_THREADS;
        }
        for (size_t j = 0; j < num_threads; j++) {
            if (writer->crash_ctx->platform.threads[j].tid == writer->tids[i]) {
                uctx = &writer->crash_ctx->platform.threads[j].context;
                break;
            }
        }

        // If we have context for this thread, write it
        if (uctx) {
            SENTRY_DEBUGF("Thread %u: writing context", thread->thread_id);
            // Write thread context
            thread->thread_context.rva
                = write_thread_context(writer, uctx, thread->thread_id);
            thread->thread_context.size
                = thread->thread_context.rva ? get_context_size() : 0;
            SENTRY_DEBUGF("Thread %u: context written at RVA 0x%x",
                thread->thread_id, thread->thread_context.rva);

            // Write stack memory
            uint64_t sp;
#    if defined(__x86_64__)
            sp = uctx->uc_mcontext.gregs[REG_RSP];
#    elif defined(__aarch64__)
            sp = uctx->uc_mcontext.sp;
#    elif defined(__i386__)
            sp = uctx->uc_mcontext.gregs[REG_ESP];
#    elif defined(__arm__)
            sp = uctx->uc_mcontext.arm_sp;
#    endif

            SENTRY_DEBUGF("Thread %u: has context, SP=0x%llx",
                thread->thread_id, (unsigned long long)sp);

            if (sp != 0) {
                size_t stack_size = 0;
                uint64_t stack_start = 0;
                thread->stack.memory.rva
                    = write_thread_stack(writer, sp, &stack_size, &stack_start);
                thread->stack.memory.size = stack_size;
                thread->stack.start_address = stack_start;
                record_thread_stack(writer, thread);

                SENTRY_DEBUGF("Thread %u: wrote context at RVA 0x%x, stack at "
                              "RVA 0x%x (size %zu)",
                    thread->thread_id, thread->thread_context.rva,
                    thread->stack.memory.rva, stack_size);
            } else {
                // SP is 0, try to get registers via ptrace
                ptrace_capture_thread(writer, thread, "SP is 0");
            }
        } else {
            // No context from signal handler - capture via ptrace.
            // On Linux, the signal handler only captures the crashing thread's
            // context. For all other threads, we need to attach via ptrace to
            // get their registers and stack memory.
            ptrace_capture_thread(
                writer, thread, "no context from signal handler");
        }
    }

    dir->stream_type = MINIDUMP_STREAM_THREAD_LIST;
    dir->rva = write_data(writer, thread_list, list_size);
    dir->data_size = list_size;

    sentry_free(thread_list);
    return dir->rva ? 0 : -1;
}

/**
 * Write module list stream (shared libraries)
 */
/**
 * Resolved module info: merges all /proc/pid/maps segments of the same ELF
 * file into a single module with correct base address and full virtual size.
 *
 * On Linux, ELF files are mapped as multiple segments:
 *   addr1 r--p offset=0x000000 /lib/foo.so   (ELF headers, rodata)
 *   addr2 r-xp offset=0x010000 /lib/foo.so   (code)
 *   addr3 r--p offset=0x030000 /lib/foo.so   (rodata)
 *   addr4 rw-p offset=0x040000 /lib/foo.so   (data/bss)
 *
 * base_of_image must be the start of the mapping with offset==0 (the real ELF
 * load address). size_of_image must span from base to end of the last segment.
 * This matches Breakpad/Crashpad behavior and is required for server-side CFI
 * unwinding to correctly compute RVAs (rva = ip - base_of_image).
 */
typedef struct {
    uint64_t base; // Start of mapping with offset==0
    uint64_t end; // End of last mapping for this file
    char *name; // Pointer into mappings[].name (not owned)
    char soname[256]; // SONAME from ELF dynamic section (empty if not found)
    uint64_t elf_size; // Size from PT_LOAD segments (0 = use maps size)
    uint8_t build_id[32];
    size_t build_id_len;
} resolved_module_t;

/**
 * Resolve all mappings into deduplicated modules.
 *
 * For each named, readable mapping that points at a real ELF file, group
 * every mapping of the same file (regardless of order in /proc/pid/maps)
 * into a single resolved_module_t. This matches Crashpad's behavior and
 * avoids two pitfalls that confuse downstream consumers (notably LLDB's
 * ModuleList on Windows reading Linux dumps):
 *
 *   1. Non-ELF entries (e.g. /dev/shm IPC files, "(deleted)" files) being
 *      emitted as modules with bogus zero-id CV records.
 *   2. The same shared library appearing twice when its segments are
 *      separated by an unrelated mapping (e.g. ld-linux-aarch64.so.1 split
 *      across non-contiguous /proc/maps lines).
 *
 * base_of_image is set to the address of the offset==0 mapping — the real
 * ELF load address — which is what Breakpad/Crashpad/rust-minidump expect.
 */
static size_t
resolve_modules(const minidump_writer_t *writer, resolved_module_t *modules,
    size_t max_modules)
{
    size_t module_count = 0;

    // Names we've already classified as non-ELF, so we don't repeatedly open
    // the same file (e.g. /dev/shm/sentry-IPC has multiple segments).
    const char *skip_names[SENTRY_CRASH_MAX_MODULES];
    size_t skip_count = 0;

    for (size_t i = 0; i < writer->mapping_count; i++) {
        const memory_mapping_t *mapping = &writer->mappings[i];

        // Skip anonymous and special kernel mappings ([stack], [vdso], etc.)
        if (mapping->name[0] == '\0' || mapping->name[0] == '[') {
            continue;
        }
        // Skip non-readable mappings
        if (mapping->permissions[0] != 'r') {
            continue;
        }

        // Already classified as non-ELF? Cheap pointer compare first
        // (mapping->name is stable for the writer's lifetime), then strcmp.
        bool already_skipped = false;
        for (size_t j = 0; j < skip_count; j++) {
            if (skip_names[j] == mapping->name
                || strcmp(skip_names[j], mapping->name) == 0) {
                already_skipped = true;
                break;
            }
        }
        if (already_skipped) {
            continue;
        }

        // Find an existing module entry by name. This merges across
        // non-contiguous /proc/maps lines — fixes duplicate ld-linux entries.
        resolved_module_t *mod = NULL;
        for (size_t j = 0; j < module_count; j++) {
            if (strcmp(modules[j].name, mapping->name) == 0) {
                mod = &modules[j];
                break;
            }
        }

        if (mod) {
            // Extend the existing module to cover this segment.
            if (mapping->end > mod->end) {
                mod->end = mapping->end;
            }
            // base_of_image is the ELF load address — the lowest mapped VA
            // for this file. In well-formed ELFs that's always the offset==0
            // PT_LOAD (the linker emits PT_LOADs in ascending p_vaddr order
            // and p_vaddr for the first one is 0), but never trust an
            // adversarial or hand-crafted ELF to do that: only move base
            // downward, never upward.
            if (mapping->start < mod->base) {
                mod->base = mapping->start;
            }
            continue;
        }

        // First time we see this file — confirm it's actually an ELF before
        // adding it as a module. This drops sentry-native's own IPC shm,
        // deleted semaphores, and any other non-ELF named mapping.
        if (!is_elf_file(mapping->name)) {
            if (skip_count < SENTRY_CRASH_MAX_MODULES) {
                skip_names[skip_count++] = mapping->name;
            }
            SENTRY_DEBUGF("skipping non-ELF mapping: %s", mapping->name);
            continue;
        }

        if (module_count >= max_modules) {
            continue;
        }

        mod = &modules[module_count++];
        memset(mod, 0, sizeof(*mod));
        mod->name = (char *)mapping->name;
        mod->base = mapping->start;
        mod->end = mapping->end;
    }

    // Extract Build IDs, ELF sizes, and SONAMEs for each resolved module
    for (size_t i = 0; i < module_count; i++) {
        modules[i].build_id_len = extract_elf_build_id(
            modules[i].name, modules[i].build_id, sizeof(modules[i].build_id));
        modules[i].elf_size = compute_elf_size_from_phdrs(modules[i].name);
        if (!read_elf_soname(modules[i].name, modules[i].soname,
                sizeof(modules[i].soname))) {
            modules[i].soname[0] = '\0';
        }
    }

    return module_count;
}

static int
write_module_list_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    SENTRY_DEBUGF("write_module_list_stream: processing %zu total mappings",
        writer->mapping_count);

    // Resolve mappings into deduplicated modules with correct base/size.
    // Use a stack-allocated array with a reasonable max
    // (SENTRY_CRASH_MAX_MODULES).
    resolved_module_t *resolved
        = sentry_malloc(sizeof(resolved_module_t) * SENTRY_CRASH_MAX_MODULES);
    if (!resolved) {
        return -1;
    }

    size_t module_count
        = resolve_modules(writer, resolved, SENTRY_CRASH_MAX_MODULES);

    size_t list_size
        = sizeof(uint32_t) + (module_count * sizeof(minidump_module_t));
    minidump_module_list_t *module_list = sentry_malloc(list_size);
    if (!module_list) {
        sentry_free(resolved);
        return -1;
    }

    module_list->count = module_count;
    SENTRY_DEBUGF("Writing %zu modules to minidump", module_count);

    // First pass: populate module list entries
    typedef struct {
        uint8_t build_id[32];
        size_t build_id_len;
        char *name;
        uint64_t base;
        uint32_t size;
    } module_info_t;
    module_info_t *mod_infos = NULL;
    if (module_count > 0) {
        mod_infos = sentry_malloc(sizeof(module_info_t) * module_count);
        if (!mod_infos) {
            sentry_free(module_list);
            sentry_free(resolved);
            return -1;
        }
    }

    for (size_t i = 0; i < module_count; i++) {
        minidump_module_t *module = &module_list->modules[i];
        memset(module, 0, sizeof(*module));

        module->base_of_image = resolved[i].base;
        module->size_of_image = resolved[i].elf_size
            ? (uint32_t)resolved[i].elf_size
            : (uint32_t)(resolved[i].end - resolved[i].base);

        // Set VS_FIXEDFILEINFO signature (first uint32_t of version_info)
        // This is required for minidump processors to recognize the module
        uint32_t version_sig = 0xFEEF04BD;
        memcpy(&module->version_info[0], &version_sig, sizeof(version_sig));

        // Store info for later writing — prefer SONAME over full path
        mod_infos[i].name
            = resolved[i].soname[0] ? resolved[i].soname : resolved[i].name;
        mod_infos[i].base = resolved[i].base;
        mod_infos[i].size = module->size_of_image;
        memcpy(mod_infos[i].build_id, resolved[i].build_id,
            sizeof(mod_infos[i].build_id));
        mod_infos[i].build_id_len = resolved[i].build_id_len;

        SENTRY_DEBUGF("Module: %s base=0x%llx size=0x%x build_id_len=%zu",
            resolved[i].name, (unsigned long long)resolved[i].base,
            mod_infos[i].size, mod_infos[i].build_id_len);
    }

    // NOTE: do NOT free `resolved` here — mod_infos[i].name may point into
    // resolved[i].soname, which lives inside the resolved array. We free it
    // after the second pass that writes module names to the minidump file.

    // Write the module list structure FIRST (with zero RVAs)
    dir->stream_type = MINIDUMP_STREAM_MODULE_LIST;
    dir->rva = write_data(writer, module_list, list_size);
    dir->data_size = list_size;

    if (dir->rva == 0) {
        SENTRY_WARN("failed to write module list structure");
        sentry_free(mod_infos);
        sentry_free(module_list);
        sentry_free(resolved);
        return -1;
    }

    // Guard against uint32_t RVA overflow. Minidump RVAs are 32-bit, so the
    // entire file must stay under 4GB. In practice this never happens, but
    // check defensively to avoid silently corrupt seek+patch writes below.
    if (writer->current_offset > UINT32_MAX) {
        SENTRY_WARN("minidump offset exceeds uint32_t range, skipping module "
                    "name/CV patching");
        sentry_free(mod_infos);
        sentry_free(module_list);
        sentry_free(resolved);
        return dir->rva ? 0 : -1;
    }

    // Second pass: write module names and CV records, then update module list
    for (size_t i = 0; i < module_count; i++) {
        // Write module name
        minidump_rva_t name_rva
            = write_minidump_string(writer, mod_infos[i].name);

        // Write CV record if we have a Build ID
        minidump_rva_t cv_rva = 0;
        uint32_t cv_size = 0;
        if (mod_infos[i].build_id_len > 0) {
            cv_rva = write_cv_record(
                writer, "", mod_infos[i].build_id, mod_infos[i].build_id_len);
            cv_size = sizeof(uint32_t) + mod_infos[i].build_id_len;
            SENTRY_DEBUGF("CV Record: signature=0x4270454c, build_id_len=%zu",
                mod_infos[i].build_id_len);
        }

        // Third pass: update specific fields in the module structure via lseek
        // Save position AFTER writing name and CV record
        off_t saved_pos = lseek(writer->fd, 0, SEEK_CUR);
        if (saved_pos == (off_t)-1) {
            SENTRY_WARNF("Failed to get current position for module %zu", i);
            continue;
        }

        // Update module_name_rva field
        off_t name_rva_offset = dir->rva + sizeof(uint32_t)
            + (i * sizeof(minidump_module_t))
            + offsetof(minidump_module_t, module_name_rva);

        if (lseek(writer->fd, name_rva_offset, SEEK_SET)
            == (off_t)name_rva_offset) {
            if (write(writer->fd, &name_rva, sizeof(name_rva))
                != sizeof(name_rva)) {
                SENTRY_WARNF(
                    "Failed to write module_name_rva for module %zu", i);
            }
        }

        // Update cv_record fields (size and rva)
        if (cv_size > 0) {
            off_t cv_offset = dir->rva + sizeof(uint32_t)
                + (i * sizeof(minidump_module_t))
                + offsetof(minidump_module_t, cv_record);

            SENTRY_DEBUGF("  Seeking to CV offset: 0x%llx for module %zu",
                (unsigned long long)cv_offset, i);

            off_t actual_offset = lseek(writer->fd, cv_offset, SEEK_SET);
            if (actual_offset == (off_t)cv_offset) {
                // Write size first, then rva (order in structure)
                ssize_t written1 = write(writer->fd, &cv_size, sizeof(cv_size));
                ssize_t written2 = write(writer->fd, &cv_rva, sizeof(cv_rva));

                if (written1 == sizeof(cv_size) && written2 == sizeof(cv_rva)) {
                    SENTRY_DEBUGF(
                        "  Updated module[%zu]: name_rva=0x%x, cv_rva=0x%x, "
                        "cv_size=%u",
                        i, name_rva, cv_rva, cv_size);
                } else {
                    SENTRY_WARNF("Failed to write CV record for module %zu: "
                                 "written1=%zd, written2=%zd",
                        i, written1, written2);
                }
            } else {
                SENTRY_WARNF("Failed to seek to CV offset 0x%llx for module "
                             "%zu (got 0x%llx)",
                    (unsigned long long)cv_offset, i,
                    (unsigned long long)actual_offset);
            }
        }

        if (lseek(writer->fd, saved_pos, SEEK_SET) != saved_pos) {
            SENTRY_WARNF(
                "Failed to restore position after module %zu update", i);
        }
    }

    // Final flush to ensure all writes are committed
    fsync(writer->fd);
    SENTRY_DEBUG("Flushed all module updates to disk");

    sentry_free(mod_infos);
    sentry_free(resolved);
    sentry_free(module_list);
    return dir->rva ? 0 : -1;
}

/**
 * Write exception stream
 */
static int
write_exception_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    minidump_exception_stream_t exception_stream = { 0 };

    exception_stream.thread_id = writer->crash_ctx->crashed_tid;

    // Use raw signal number as exception code, matching Breakpad/Crashpad
    // convention for Linux minidumps. lldb and other debuggers expect the
    // signal number directly (e.g. 11 for SIGSEGV), not a Windows-style
    // exception code.
    exception_stream.exception_record.exception_code
        = writer->crash_ctx->platform.signum;
    // Set exception_flags to si_code, matching Crashpad behavior
    // (e.g., SEGV_MAPERR for SIGSEGV address not mapped to object)
    exception_stream.exception_record.exception_flags
        = (uint32_t)writer->crash_ctx->platform.siginfo.si_code;
    exception_stream.exception_record.exception_address
        = (uint64_t)writer->crash_ctx->platform.siginfo.si_addr;
    exception_stream.exception_record.number_parameters = 0;

    // Write the crashing thread's context
    const ucontext_t *uctx = &writer->crash_ctx->platform.context;
    exception_stream.thread_context.rva
        = write_thread_context(writer, uctx, writer->crash_ctx->crashed_tid);
    exception_stream.thread_context.size
        = exception_stream.thread_context.rva ? get_context_size() : 0;

    SENTRY_DEBUGF("Exception: wrote context at RVA 0x%x for thread %u",
        exception_stream.thread_context.rva, exception_stream.thread_id);

    dir->stream_type = MINIDUMP_STREAM_EXCEPTION;
    dir->rva = write_data(writer, &exception_stream, sizeof(exception_stream));
    dir->data_size = sizeof(exception_stream);

    return dir->rva ? 0 : -1;
}

/**
 * Write thread names stream (stream type 24).
 * Matches Crashpad's ThreadNamesStream format: a list of
 * MINIDUMP_THREAD_NAME entries, each pointing to a UTF-16LE string.
 */
static int
write_thread_names_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    SENTRY_DEBUGF(
        "write_thread_names_stream: %zu threads", writer->thread_count);

    // First pass: write all thread name strings and collect their RVAs
    minidump_rva_t *name_rvas
        = sentry_malloc(sizeof(minidump_rva_t) * writer->thread_count);
    if (!name_rvas) {
        return -1;
    }

    for (size_t i = 0; i < writer->thread_count; i++) {
        const char *name = writer->thread_names[i];
        if (name[0] != '\0') {
            name_rvas[i] = write_minidump_string(writer, name);
        } else {
            // Write empty string for threads without names
            name_rvas[i] = write_minidump_string(writer, "");
        }
    }

    // Second pass: write the thread names list structure
    size_t list_size = sizeof(uint32_t)
        + (writer->thread_count * sizeof(minidump_thread_name_t));
    minidump_thread_name_list_t *name_list = sentry_malloc(list_size);
    if (!name_list) {
        sentry_free(name_rvas);
        return -1;
    }

    name_list->count = writer->thread_count;
    for (size_t i = 0; i < writer->thread_count; i++) {
        name_list->thread_names[i].thread_id = writer->tids[i];
        name_list->thread_names[i].thread_name_rva = name_rvas[i];
    }

    // Stage rva first so a write_data failure leaves dir untouched
    // (data_size > 0 with rva == 0 would point parsers at the header).
    minidump_rva_t rva = write_data(writer, name_list, list_size);
    sentry_free(name_list);
    sentry_free(name_rvas);
    if (!rva) {
        return -1;
    }
    dir->stream_type = MINIDUMP_STREAM_THREAD_NAMES;
    dir->rva = rva;
    dir->data_size = list_size;
    return 0;
}

/**
 * Check if a memory region should be included based on minidump mode
 */
static bool
should_include_region(const memory_mapping_t *mapping,
    sentry_minidump_mode_t mode, uint64_t crash_addr)
{
    // STACK_ONLY: Only include stack regions (captured in thread list already)
    if (mode == SENTRY_MINIDUMP_MODE_STACK_ONLY) {
        return false; // Thread list already has stack memory
    }

    // FULL: Include all readable regions
    if (mode == SENTRY_MINIDUMP_MODE_FULL) {
        return mapping->permissions[0] == 'r'; // Must be readable
    }

    // SMART: Include only regions containing crash address and named heap
    // We do NOT include all anonymous rw regions as that captures too much
    // memory (thread stacks, mmap allocations, etc.) and results in huge
    // minidumps (34MB+). Stack memory is already captured per-thread in the
    // thread list stream, so we only need heap data that's directly relevant.
    if (mode == SENTRY_MINIDUMP_MODE_SMART) {
        // Include regions containing crash address (most important)
        if (crash_addr >= mapping->start && crash_addr < mapping->end) {
            return mapping->permissions[0] == 'r';
        }

        // Include the main heap region (explicitly named [heap])
        // Limit to 4MB to avoid huge dumps
        if (strstr(mapping->name, "[heap]") != NULL) {
            return mapping->permissions[0] == 'r'
                && (mapping->end - mapping->start) <= (4 * 1024 * 1024);
        }

        // Include first page of loaded modules (ELF headers) for offline
        // symbolication, matching breakpad/crashpad behavior. Debuggers need
        // these to identify modules when symbol files aren't available locally.
        if (mapping->offset == 0 && mapping->name[0] != '\0'
            && mapping->name[0] != '[' && mapping->permissions[0] == 'r') {
            return true;
        }

        // Don't include anonymous regions - they're typically thread stacks
        // (already captured), mmap'd memory, or large allocations that would
        // bloat the minidump. Windows' MiniDumpWithIndirectlyReferencedMemory
        // is much smarter about what to include (~683KB vs 34MB).
    }

    return false;
}

// =====================================================================
// Indirectly-referenced memory capture (SMART mode)
// =====================================================================
//
// Mirrors Windows' MiniDumpWithIndirectlyReferencedMemory: walks every
// captured thread's stack words and the crashing thread's GPRs and, for
// each value that lands inside a writable heap mapping, captures a small
// page-aligned chunk so debuggers can chase pointers held in struct
// locals at crash time.
//
// The algorithm + accumulator + dedup live in sentry_minidump_indirect.c.
// This block is the Linux platform shim: it provides the two callbacks
// (is_writable_heap, read_memory) and drives the walker for each thread.

/**
 * Find the mapping containing addr via binary search over writer->mappings,
 * which is sorted by /proc/<pid>/maps in ascending start order. Returns NULL
 * if addr is not mapped.
 */
static const memory_mapping_t *
find_mapping_for_addr(const minidump_writer_t *writer, uint64_t addr)
{
    size_t lo = 0;
    size_t hi = writer->mapping_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const memory_mapping_t *m = &writer->mappings[mid];
        if (addr < m->start) {
            hi = mid;
        } else if (addr >= m->end) {
            lo = mid + 1;
        } else {
            return m;
        }
    }
    return NULL;
}

static bool
linux_indirect_is_writable_heap(void *ctx, uint64_t addr)
{
    const minidump_writer_t *writer = (const minidump_writer_t *)ctx;
    const memory_mapping_t *m = find_mapping_for_addr(writer, addr);
    if (!m) {
        return false;
    }
    // Must be readable+writable, not executable. This admits [heap] and
    // anonymous rw-p mappings (typical heap allocators); rejects code
    // pages, rwx JIT pages (rare and risky), and read-only data segments.
    if (m->permissions[0] != 'r' || m->permissions[1] != 'w') {
        return false;
    }
    if (m->permissions[2] == 'x') {
        return false;
    }
    // Reject kernel-private/special regions: [stack...], [vdso], [vvar],
    // [vsyscall]. Stacks are already in MemoryListStream via thread_stacks
    // so visiting them here would just waste budget on dedup work.
    if (m->name[0] == '[') {
        // [heap] is the one named-bracket region we DO want.
        if (strncmp(m->name, "[heap]", 6) == 0) {
            return true;
        }
        return false;
    }
    return true;
}

static ssize_t
linux_indirect_read_memory(void *ctx, uint64_t addr, void *buf, size_t len)
{
    // Pointer-chasing is best-effort: use process_vm_readv only, skip the
    // ptrace PEEKDATA fallback. Under sanitizers (notably TSAN) the syscall
    // can return EPERM intermittently; falling back would then issue one
    // ptrace syscall per word — for ~100 captured regions × 512 words each,
    // that's 50K instrumented syscalls per dump and minutes of crash-time
    // latency. If the fast path can't read, just drop the candidate; the
    // walker treats failed reads as "not capturable" and moves on.
    minidump_writer_t *writer = (minidump_writer_t *)ctx;
    if (!ptrace_attach_process(writer)) {
        return -1;
    }
    pid_t tid = writer->crash_ctx->crashed_tid;
    struct iovec local_iov = { .iov_base = buf, .iov_len = len };
    struct iovec remote_iov = { .iov_base = (void *)addr, .iov_len = len };
    return process_vm_readv(tid, &local_iov, 1, &remote_iov, 1, 0);
}

/**
 * Walk the registers of the captured ucontext for `tid` (if available)
 * through sentry__indirect_consider. The crash daemon only stores ucontext
 * for threads whose context arrived via the signal handler — typically the
 * crashing thread plus any other thread that hit the same handler. For
 * threads captured via ptrace the registers are already gone by the time
 * we get here, so we silently skip them; their stack contents still get
 * walked below.
 */
static void
walk_indirect_registers_for_tid(minidump_writer_t *writer, pid_t tid,
    sentry_indirect_accumulator_t *acc, const sentry_indirect_ops_t *ops)
{
    const ucontext_t *uctx = NULL;
    size_t num = writer->crash_ctx->platform.num_threads;
    if (num > SENTRY_CRASH_MAX_THREADS) {
        num = SENTRY_CRASH_MAX_THREADS;
    }
    for (size_t j = 0; j < num; j++) {
        if (writer->crash_ctx->platform.threads[j].tid == tid) {
            uctx = &writer->crash_ctx->platform.threads[j].context;
            break;
        }
    }
    if (!uctx) {
        return;
    }

    minidump_writer_base_t *base = (minidump_writer_base_t *)writer;
#    if defined(__aarch64__)
    for (int r = 0; r <= 30; r++) {
        sentry__indirect_consider(
            acc, base, (uint64_t)uctx->uc_mcontext.regs[r], ops);
    }
    sentry__indirect_consider(acc, base, (uint64_t)uctx->uc_mcontext.sp, ops);
#    elif defined(__x86_64__)
    static const int x86_64_gprs[] = { REG_RAX, REG_RBX, REG_RCX, REG_RDX,
        REG_RSI, REG_RDI, REG_RBP, REG_RSP, REG_R8, REG_R9, REG_R10, REG_R11,
        REG_R12, REG_R13, REG_R14, REG_R15 };
    for (size_t i = 0; i < sizeof(x86_64_gprs) / sizeof(x86_64_gprs[0]); i++) {
        sentry__indirect_consider(
            acc, base, (uint64_t)uctx->uc_mcontext.gregs[x86_64_gprs[i]], ops);
    }
#    elif defined(__i386__)
    static const int x86_gprs[] = { REG_EAX, REG_EBX, REG_ECX, REG_EDX, REG_ESI,
        REG_EDI, REG_EBP, REG_ESP };
    for (size_t i = 0; i < sizeof(x86_gprs) / sizeof(x86_gprs[0]); i++) {
        sentry__indirect_consider(acc, base,
            (uint64_t)(uint32_t)uctx->uc_mcontext.gregs[x86_gprs[i]], ops);
    }
#    elif defined(__arm__)
    // ARMv7's mcontext fields are individual struct members rather than
    // an array; walk them explicitly. Most heap pointers we care about
    // either come through r0..r3 (call args / return value), r7 (frame
    // pointer in some calling conventions), sp, or lr.
    sentry__indirect_consider(
        acc, base, (uint64_t)uctx->uc_mcontext.arm_r0, ops);
    sentry__indirect_consider(
        acc, base, (uint64_t)uctx->uc_mcontext.arm_r1, ops);
    sentry__indirect_consider(
        acc, base, (uint64_t)uctx->uc_mcontext.arm_r2, ops);
    sentry__indirect_consider(
        acc, base, (uint64_t)uctx->uc_mcontext.arm_r3, ops);
    sentry__indirect_consider(
        acc, base, (uint64_t)uctx->uc_mcontext.arm_r7, ops);
    sentry__indirect_consider(
        acc, base, (uint64_t)uctx->uc_mcontext.arm_sp, ops);
    sentry__indirect_consider(
        acc, base, (uint64_t)uctx->uc_mcontext.arm_lr, ops);
#    endif
}

/**
 * For SMART mode: scan every captured thread's stack and the crashing
 * thread's registers for heap pointers, capturing a chunk around each.
 * Stack bytes are pread()'d back from disk (we already wrote them in
 * write_thread_list_stream) rather than retained in memory, to avoid
 * doubling crash-time memory pressure.
 */
static void
capture_indirect_memory(
    minidump_writer_t *writer, sentry_indirect_accumulator_t *acc)
{
    sentry_indirect_ops_t ops = {
        .is_writable_heap = linux_indirect_is_writable_heap,
        .read_memory = linux_indirect_read_memory,
        .ctx = writer,
    };
    minidump_writer_base_t *base = (minidump_writer_base_t *)writer;

    for (size_t i = 0; i < writer->thread_stack_count; i++) {
        const minidump_memory_descriptor_t *desc = &writer->thread_stacks[i];
        size_t stack_size = desc->memory.size;
        if (stack_size == 0 || desc->memory.rva == 0) {
            continue;
        }
        void *stack_buf = sentry_malloc(stack_size);
        if (!stack_buf) {
            continue;
        }
        ssize_t got
            = pread(writer->fd, stack_buf, stack_size, (off_t)desc->memory.rva);
        if (got > 0) {
            sentry__indirect_walk_words(
                acc, base, stack_buf, (size_t)got, &ops);
        }
        sentry_free(stack_buf);

        // After scanning this thread's stack, also scan its registers if
        // we have them. Registers usually point into either the same stack
        // (already walked) or live heap objects we want to capture.
        walk_indirect_registers_for_tid(
            writer, writer->thread_stack_tids[i], acc, &ops);

        if (acc->total_bytes >= SENTRY_INDIRECT_MAX_TOTAL_BYTES
            || acc->region_count >= SENTRY_INDIRECT_MAX_REGIONS) {
            break;
        }
    }
}

/**
 * Write memory list stream (heap memory based on minidump mode)
 */
static int
write_memory_list_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    // Get crash address for SMART mode filtering
    uint64_t crash_addr = (uint64_t)writer->crash_ctx->platform.siginfo.si_addr;

    // SMART mode: scan registers + stack words for pointers into writable
    // heap mappings and capture a chunk around each. Has to run BEFORE the
    // memory_list allocation below so we know the final region count. The
    // bytes are written into the dump file as we go; we only collect the
    // descriptors here and merge them into the unified list at the end.
    sentry_indirect_accumulator_t indirect_acc;
    sentry__indirect_init(&indirect_acc);
    if (writer->crash_ctx->minidump_mode == SENTRY_MINIDUMP_MODE_SMART) {
        capture_indirect_memory(writer, &indirect_acc);
        SENTRY_DEBUGF("indirect memory: captured %zu regions, %zu bytes",
            indirect_acc.region_count, indirect_acc.total_bytes);
    }

    // Count regions to include based on mode
    size_t region_count = 0;
    for (size_t i = 0; i < writer->mapping_count; i++) {
        if (should_include_region(&writer->mappings[i],
                writer->crash_ctx->minidump_mode, crash_addr)) {
            region_count++;
        }
    }

    // Reserve slots for thread stacks first; their bytes are already on disk
    // (written by the thread-list writer) so we just emit descriptors
    // pointing at the same RVA. This is what lets LLDB resolve memory at
    // stack addresses and walk the FP chain.
    size_t total_count
        = region_count + writer->thread_stack_count + indirect_acc.region_count;

    // Allocate memory list
    size_t list_size = sizeof(uint32_t)
        + (total_count * sizeof(minidump_memory_descriptor_t));
    minidump_memory_list_t *memory_list = sentry_malloc(list_size);
    if (!memory_list) {
        return -1;
    }

    memory_list->count = total_count;

    size_t mem_idx = 0;
    // Replay thread stacks first so they appear in deterministic order. Each
    // descriptor was already validated (size>0, rva!=0) by record_thread_stack.
    for (size_t s = 0; s < writer->thread_stack_count; s++) {
        memory_list->ranges[mem_idx++] = writer->thread_stacks[s];
    }

    // Append indirectly-referenced regions (heap chunks pointed to by
    // registers/stack contents). Already deduped + capped by the walker.
    for (size_t s = 0; s < indirect_acc.region_count; s++) {
        minidump_memory_descriptor_t *mem = &memory_list->ranges[mem_idx++];
        mem->start_address = indirect_acc.regions[s].start;
        mem->memory.rva = indirect_acc.regions[s].rva;
        mem->memory.size = indirect_acc.regions[s].size;
    }

    // Write memory regions
    for (size_t i = 0; i < writer->mapping_count && mem_idx < total_count;
        i++) {
        if (!should_include_region(&writer->mappings[i],
                writer->crash_ctx->minidump_mode, crash_addr)) {
            continue;
        }

        memory_mapping_t *mapping = &writer->mappings[i];
        minidump_memory_descriptor_t *mem = &memory_list->ranges[mem_idx++];

        uint64_t region_size = mapping->end - mapping->start;

        // For SMART mode, cap module header pages to one page (4096 bytes).
        // We only need the ELF header, not the entire read-only segment.
        // Skip the cap if this region contains the crash address, since
        // that memory is the most important for debugging.
        if (writer->crash_ctx->minidump_mode == SENTRY_MINIDUMP_MODE_SMART
            && mapping->offset == 0 && mapping->name[0] != '\0'
            && mapping->name[0] != '['
            && !(crash_addr >= mapping->start && crash_addr < mapping->end)) {
            const uint64_t MODULE_HEADER_SIZE = 4096;
            if (region_size > MODULE_HEADER_SIZE) {
                region_size = MODULE_HEADER_SIZE;
            }
        }

        // Limit individual region size to 4MB to avoid huge dumps
        // (should_include_region already limits heap to 4MB for SMART mode)
        const size_t MAX_REGION_SIZE = 4 * 1024 * 1024; // 4MB
        if (region_size > MAX_REGION_SIZE) {
            region_size = MAX_REGION_SIZE;
        }

        // Allocate buffer for region memory
        void *region_buffer = sentry_malloc(region_size);
        if (!region_buffer) {
            mem->start_address = mapping->start;
            mem->memory.size = 0;
            mem->memory.rva = 0;
            continue;
        }

        // Read memory from crashed process
        ssize_t nread = read_process_memory(
            writer, mapping->start, region_buffer, region_size);

        if (nread > 0) {
            mem->start_address = mapping->start;
            mem->memory.rva = write_data(writer, region_buffer, nread);
            mem->memory.size = mem->memory.rva ? (uint32_t)nread : 0;
        } else {
            mem->start_address = mapping->start;
            mem->memory.size = 0;
            mem->memory.rva = 0;
        }

        sentry_free(region_buffer);
    }

    dir->stream_type = MINIDUMP_STREAM_MEMORY_LIST;
    dir->rva = write_data(writer, memory_list, list_size);
    dir->data_size = list_size;

    sentry_free(memory_list);
    return dir->rva ? 0 : -1;
}

/**
 * Slurp a file into a freshly allocated buffer.
 *
 * Used for /proc/<pid>/{auxv,cmdline,environ,maps,status}, /proc/cpuinfo,
 * and /etc/lsb-release. Those are kernel seqfiles or short text files that
 * cannot be stat()'d for size (st_size is 0), so we read until EOF, doubling
 * the buffer up to `max_size`. Caller owns the returned pointer.
 *
 * Returns the buffer on success and writes its length to *size_out.
 * Returns NULL on open/read failure or when the file is empty.
 */
static void *
read_proc_file(const char *path, size_t max_size, size_t *size_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    size_t buf_size = 4096;
    if (buf_size > max_size) {
        buf_size = max_size;
    }
    char *buf = sentry_malloc(buf_size);
    if (!buf) {
        close(fd);
        return NULL;
    }

    size_t total = 0;
    ssize_t n;
    for (;;) {
        if (total >= buf_size) {
            if (buf_size >= max_size) {
                break;
            }
            size_t new_size = buf_size * 2;
            if (new_size > max_size) {
                new_size = max_size;
            }
            char *new_buf = sentry_malloc(new_size);
            if (!new_buf) {
                break;
            }
            memcpy(new_buf, buf, total);
            sentry_free(buf);
            buf = new_buf;
            buf_size = new_size;
        }

        n = read(fd, buf + total, buf_size - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    close(fd);

    if (total == 0) {
        sentry_free(buf);
        return NULL;
    }
    *size_out = total;
    return buf;
}

/**
 * Helper: write a /proc file (or any other path) as a single stream.
 * Returns 0 on success, -1 on any failure (including file unavailable).
 *
 * On failure (read or write_data) the directory entry is fully zeroed —
 * a non-zero data_size paired with rva=0 would point parsers at the
 * minidump header.
 */
static int
write_file_as_stream(minidump_writer_t *writer, minidump_directory_t *dir,
    uint32_t stream_type, const char *path, size_t max_size)
{
    size_t size = 0;
    void *buf = read_proc_file(path, max_size, &size);
    if (!buf) {
        return -1;
    }

    minidump_rva_t rva = write_data(writer, buf, size);
    sentry_free(buf);
    if (!rva) {
        return -1;
    }
    dir->stream_type = stream_type;
    dir->rva = rva;
    dir->data_size = (uint32_t)size;
    return 0;
}

/**
 * Write /proc/PID/status as LinuxProcStatus stream.
 * lldb reads this to get the process ID (Pid: line).
 */
static int
write_linux_proc_status_stream(
    minidump_writer_t *writer, minidump_directory_t *dir)
{
    char path[64];
    snprintf(
        path, sizeof(path), "/proc/%d/status", writer->crash_ctx->crashed_pid);
    return write_file_as_stream(
        writer, dir, MINIDUMP_STREAM_LINUX_PROC_STATUS, path, 64 * 1024);
}

/**
 * Write /proc/PID/auxv as LinuxAuxv stream.
 *
 * The auxiliary vector is a sequence of (Elf{32,64}_auxv_t) entries the
 * kernel hands the dynamic loader at process start. LLDB's
 * ProcessMinidump uses AT_PHDR/AT_PHNUM/AT_ENTRY from this stream to
 * locate the main executable and r_debug; without it, LLDB on Linux
 * falls back to a degraded unwind path that on AArch64 stops after the
 * first frame.
 */
static int
write_linux_auxv_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    char path[64];
    snprintf(
        path, sizeof(path), "/proc/%d/auxv", writer->crash_ctx->crashed_pid);
    return write_file_as_stream(
        writer, dir, MINIDUMP_STREAM_LINUX_AUXV, path, 16 * 1024);
}

/**
 * Write /proc/cpuinfo as LinuxCpuInfo stream. Diagnostic only.
 */
static int
write_linux_cpu_info_stream(
    minidump_writer_t *writer, minidump_directory_t *dir)
{
    return write_file_as_stream(writer, dir, MINIDUMP_STREAM_LINUX_CPU_INFO,
        "/proc/cpuinfo", 64 * 1024);
}

/**
 * Write /etc/lsb-release as LinuxLsbRelease stream. Diagnostic only;
 * /etc/lsb-release is missing on many distros and that's fine.
 */
static int
write_linux_lsb_release_stream(
    minidump_writer_t *writer, minidump_directory_t *dir)
{
    return write_file_as_stream(writer, dir, MINIDUMP_STREAM_LINUX_LSB_RELEASE,
        "/etc/lsb-release", 4 * 1024);
}

/**
 * Write /proc/PID/cmdline as LinuxCmdLine stream.
 * Argv is NUL-separated; consumers (rust-minidump, LLDB) treat it as a blob.
 */
static int
write_linux_cmd_line_stream(
    minidump_writer_t *writer, minidump_directory_t *dir)
{
    char path[64];
    snprintf(
        path, sizeof(path), "/proc/%d/cmdline", writer->crash_ctx->crashed_pid);
    return write_file_as_stream(
        writer, dir, MINIDUMP_STREAM_LINUX_CMD_LINE, path, 32 * 1024);
}

/**
 * Write /proc/PID/environ as LinuxEnviron stream.
 */
static int
write_linux_environ_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    char path[64];
    snprintf(
        path, sizeof(path), "/proc/%d/environ", writer->crash_ctx->crashed_pid);
    return write_file_as_stream(
        writer, dir, MINIDUMP_STREAM_LINUX_ENVIRON, path, 64 * 1024);
}

/**
 * Write /proc/PID/maps as LinuxMaps stream.
 * lldb uses this for memory region info and module resolution.
 */
static int
write_linux_maps_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    char path[64];
    snprintf(
        path, sizeof(path), "/proc/%d/maps", writer->crash_ctx->crashed_pid);
    return write_file_as_stream(
        writer, dir, MINIDUMP_STREAM_LINUX_MAPS, path, 1024 * 1024);
}

// Native (host = target) link-map / debug-struct types for the DSO debug
// stream. We don't support cross-bitness dumps, so picking by host pointer
// size is correct. The 32-bit variants match Breakpad's MDRawLinkMap32 /
// MDRawDebug32 (12 / 24 bytes); the 64-bit variants match MDRawLinkMap64
// / MDRawDebug64 (20 / 36 bytes).
#    if __SIZEOF_POINTER__ == 4
#        define DSO_LINK_MAP_T minidump_link_map32_t
#        define DSO_DEBUG_T minidump_debug32_t
#        define DSO_UINT_T uint32_t
#    else
#        define DSO_LINK_MAP_T minidump_link_map64_t
#        define DSO_DEBUG_T minidump_debug64_t
#        define DSO_UINT_T uint64_t
#    endif

/**
 * Walk the dynamic linker's r_debug structure and write it as a
 * LinuxDsoDebug stream (MD_LINUX_DSO_DEBUG / 0x4767000A).
 *
 * Tools that consume Linux minidumps — notably LLDB's ProcessMinidump on
 * AArch64 — use this stream to enumerate every loaded DSO in load order
 * with its load address (l_addr) and PT_DYNAMIC pointer (l_ld). When this
 * stream is missing, LLDB cannot locate eh_frame for the modules and
 * unwinding stops after the first frame.
 *
 * Logic mirrors Breakpad's MinidumpWriter::WriteDSODebugStream:
 *   1. Read /proc/<pid>/auxv → AT_PHDR (program headers vaddr in target),
 *      AT_PHNUM (count).
 *   2. Compute the executable load base (page-align AT_PHDR, subtract
 *      p_vaddr of the PT_LOAD with file offset 0).
 *   3. Walk program headers to find PT_DYNAMIC's vaddr.
 *   4. Walk PT_DYNAMIC entries to find DT_DEBUG → r_debug pointer.
 *   5. Walk r_debug.r_map (link_map list) and emit one
 *      minidump_link_map{32,64}_t per loaded DSO (depending on host
 *      bitness — 32-bit Linux ARM / i386 emit the 32-bit variant).
 *
 * All target-process reads go through read_process_memory (process_vm_readv
 * with ptrace fallback); we never dereference target pointers directly.
 */
static int
write_linux_dso_debug_stream(
    minidump_writer_t *writer, minidump_directory_t *dir)
{
    // Step 1: read auxv to find AT_PHDR and AT_PHNUM.
    char auxv_path[64];
    snprintf(auxv_path, sizeof(auxv_path), "/proc/%d/auxv",
        writer->crash_ctx->crashed_pid);
    size_t auxv_size = 0;
    void *auxv_buf = read_proc_file(auxv_path, 16 * 1024, &auxv_size);
    if (!auxv_buf) {
        SENTRY_DEBUG("dso_debug: could not read auxv");
        return -1;
    }

    uint64_t at_phdr = 0;
    uint64_t at_phnum = 0;
    uint64_t at_entry = 0;
    uint64_t at_base = 0; // dynamic linker load address
    const ElfW(auxv_t) *auxv = (const ElfW(auxv_t) *)auxv_buf;
    size_t auxv_count = auxv_size / sizeof(ElfW(auxv_t));
    for (size_t i = 0; i < auxv_count; i++) {
        if (auxv[i].a_type == AT_NULL) {
            break;
        }
        switch (auxv[i].a_type) {
        case AT_PHDR:
            at_phdr = auxv[i].a_un.a_val;
            break;
        case AT_PHNUM:
            at_phnum = auxv[i].a_un.a_val;
            break;
        case AT_ENTRY:
            at_entry = auxv[i].a_un.a_val;
            break;
        case AT_BASE:
            at_base = auxv[i].a_un.a_val;
            break;
        }
    }
    sentry_free(auxv_buf);

    if (!at_phdr || !at_phnum) {
        SENTRY_DEBUGF(
            "dso_debug: missing AT_PHDR/AT_PHNUM (phdr=0x%llx, phnum=%llu)",
            (unsigned long long)at_phdr, (unsigned long long)at_phnum);
        return -1;
    }

    // Step 2 + 3: read program headers, locate PT_DYNAMIC and adjust base.
    // Cap phnum to a sane upper bound so a corrupt auxv can't drive a huge
    // alloc.
    if (at_phnum > 256) {
        SENTRY_WARNF("dso_debug: phnum=%llu exceeds cap, refusing",
            (unsigned long long)at_phnum);
        return -1;
    }
    size_t phdr_size = (size_t)at_phnum * sizeof(ElfW(Phdr));
    ElfW(Phdr) *phdrs = sentry_malloc(phdr_size);
    if (!phdrs) {
        return -1;
    }
    if (read_process_memory(writer, at_phdr, phdrs, phdr_size)
        != (ssize_t)phdr_size) {
        SENTRY_DEBUG("dso_debug: failed to read program headers");
        sentry_free(phdrs);
        return -1;
    }

    // Page-align AT_PHDR for the initial base estimate; the PT_LOAD with
    // p_offset==0 then tells us the real load offset.
    uint64_t base = at_phdr & ~((uint64_t)0xfff);
    uint64_t dyn_vaddr = 0;
    size_t dyn_filesz = 0;
    for (uint64_t i = 0; i < at_phnum; i++) {
        const ElfW(Phdr) *ph = &phdrs[i];
        if (ph->p_type == PT_LOAD && ph->p_offset == 0) {
            base -= ph->p_vaddr;
        }
        if (ph->p_type == PT_DYNAMIC) {
            dyn_vaddr = ph->p_vaddr;
            dyn_filesz = ph->p_filesz;
        }
    }
    sentry_free(phdrs);

    if (!dyn_vaddr) {
        SENTRY_DEBUG("dso_debug: no PT_DYNAMIC found");
        return -1;
    }

    uint64_t dynamic_addr = dyn_vaddr + base;

    // Step 4: walk PT_DYNAMIC for DT_DEBUG.
    // dyn_filesz can be 0 in rare ELFs; cap iteration with a sane upper bound.
    size_t max_dyn_entries = dyn_filesz ? dyn_filesz / sizeof(ElfW(Dyn)) : 4096;
    if (max_dyn_entries > 4096) {
        max_dyn_entries = 4096;
    }

    uint64_t r_debug_addr = 0;
    size_t dynamic_length = 0;
    ElfW(Dyn) dyn;
    for (size_t i = 0; i < max_dyn_entries; i++) {
        uint64_t entry_addr = dynamic_addr + i * sizeof(dyn);
        if (read_process_memory(writer, entry_addr, &dyn, sizeof(dyn))
            != (ssize_t)sizeof(dyn)) {
            SENTRY_DEBUGF("dso_debug: failed to read PT_DYNAMIC entry %zu", i);
            return -1;
        }
        dynamic_length += sizeof(dyn);
        if (dyn.d_tag == DT_DEBUG) {
            r_debug_addr = (uint64_t)dyn.d_un.d_ptr;
        } else if (dyn.d_tag == DT_NULL) {
            break;
        }
    }

    // Step 5: walk r_debug.r_map (struct link_map list).
    // We declare local copies of struct r_debug / struct link_map fields
    // explicitly so we don't depend on their exact glibc layout — the
    // ABI for these is stable across glibc/musl: r_version (int) at offset 0,
    // r_map (link_map*) at offset of pointer-aligned 1, then r_brk, r_state,
    // r_ldbase. struct link_map starts with l_addr, l_name, l_ld, l_next,
    // l_prev. Using <link.h>'s definition is fine since target and dumper share
    // libc.
    struct r_debug rd;
    memset(&rd, 0, sizeof(rd));
    bool have_rdebug = false;
    if (r_debug_addr) {
        if (read_process_memory(writer, r_debug_addr, &rd, sizeof(rd))
            == (ssize_t)sizeof(rd)) {
            have_rdebug = true;
        } else {
            SENTRY_DEBUG("dso_debug: failed to read r_debug");
        }
    }

    // Count and walk DSOs. Cap at SENTRY_CRASH_MAX_MAPPINGS to bound work.
    size_t dso_count = 0;
    void *next = have_rdebug ? rd.r_map : NULL;
    while (next && dso_count < SENTRY_CRASH_MAX_MAPPINGS) {
        struct link_map lm;
        if (read_process_memory(
                writer, (uint64_t)(uintptr_t)next, &lm, sizeof(lm))
            != (ssize_t)sizeof(lm)) {
            SENTRY_DEBUGF("dso_debug: failed to read link_map at %p", next);
            break;
        }
        dso_count++;
        next = lm.l_next;
    }

    // Pre-emit the link_map array header so we have a stable RVA, then
    // stream entries. We'll backfill the array.
    size_t links_size = dso_count * sizeof(DSO_LINK_MAP_T);
    DSO_LINK_MAP_T *links = NULL;
    minidump_rva_t links_rva = 0;
    size_t links_written = 0;
    if (dso_count > 0) {
        links = sentry_malloc(links_size);
        if (!links) {
            return -1;
        }
        memset(links, 0, links_size);

        // Walk again, this time emitting strings (which share the same RVA
        // space as the link_map array — strings get written first, then the
        // array of fixed-size entries last so its RVA is contiguous).
        next = rd.r_map;
        while (next && links_written < dso_count) {
            struct link_map lm;
            if (read_process_memory(
                    writer, (uint64_t)(uintptr_t)next, &lm, sizeof(lm))
                != (ssize_t)sizeof(lm)) {
                break;
            }

            char namebuf[257] = { 0 };
            if (lm.l_name) {
                ssize_t got = read_process_memory(writer,
                    (uint64_t)(uintptr_t)lm.l_name, namebuf,
                    sizeof(namebuf) - 1);
                if (got <= 0) {
                    namebuf[0] = '\0';
                } else {
                    namebuf[got] = '\0'; // ensure NUL-terminated
                }
            }

            links[links_written].addr = (DSO_UINT_T)lm.l_addr;
            links[links_written].name = write_minidump_string(writer, namebuf);
            links[links_written].ld = (DSO_UINT_T)(uintptr_t)lm.l_ld;
            links_written++;
            next = lm.l_next;
        }

        // Only emit the entries we actually populated. Trailing zero entries
        // (from the initial memset) would point parsers at the minidump
        // header via name=rva 0, so use links_written rather than dso_count
        // to size the array on disk and in the header.
        if (links_written > 0) {
            links_rva = write_data(
                writer, links, links_written * sizeof(DSO_LINK_MAP_T));
        }
        sentry_free(links);
        if (links_written > 0 && !links_rva) {
            return -1;
        }
    }

    // Now write the MD_LINUX_DSO_DEBUG header followed by a copy of the
    // PT_DYNAMIC blob (matches Breakpad's layout: header is at the stream
    // RVA, and the dynamic-section bytes follow contiguously).
    size_t header_size = sizeof(DSO_DEBUG_T);
    size_t total_size = header_size + dynamic_length;
    uint8_t *blob = sentry_malloc(total_size);
    if (!blob) {
        return -1;
    }
    memset(blob, 0, total_size);
    DSO_DEBUG_T *hdr = (DSO_DEBUG_T *)blob;
    hdr->version = have_rdebug ? rd.r_version : 0;
    hdr->map = links_rva;
    hdr->dso_count = (uint32_t)links_written;
    hdr->brk = have_rdebug ? (DSO_UINT_T)rd.r_brk : 0;
    hdr->ldbase = (DSO_UINT_T)at_base; // matches Breakpad: AT_BASE from auxv
    hdr->dynamic = (DSO_UINT_T)dynamic_addr;

    // Best-effort: copy the actual PT_DYNAMIC bytes. Failure here is OK —
    // consumers only need the header for module enumeration.
    if (dynamic_length) {
        ssize_t got = read_process_memory(
            writer, dynamic_addr, blob + header_size, dynamic_length);
        if (got < 0) {
            // Whole read failed; keep the zeros from memset above.
        } else if ((size_t)got < dynamic_length) {
            // Partial read (ptrace fallback path can return short reads).
            // Keep what we got and zero only the unread tail.
            memset(blob + header_size + (size_t)got, 0, dynamic_length - got);
        }
        // got == dynamic_length: the whole section is in place.
    }

    // Stage rva first so a write_data failure leaves dir untouched
    // (data_size > 0 with rva == 0 would point parsers at the header).
    minidump_rva_t rva = write_data(writer, blob, total_size);
    sentry_free(blob);
    if (!rva) {
        return -1;
    }
    dir->stream_type = MINIDUMP_STREAM_LINUX_DSO_DEBUG;
    dir->rva = rva;
    dir->data_size = (uint32_t)total_size;

    SENTRY_DEBUGF("dso_debug: wrote %zu DSOs (dynamic@0x%llx, r_debug@0x%llx)",
        links_written, (unsigned long long)dynamic_addr,
        (unsigned long long)r_debug_addr);
    (void)at_entry; // currently unused; logged via auxv for debugging
    return 0;
}

/**
 * Main minidump writing function for Linux
 */
int
sentry__write_minidump(
    const sentry_crash_context_t *ctx, const char *output_path)
{
    SENTRY_DEBUGF("writing minidump to %s", output_path);
    SENTRY_DEBUGF("crashed_pid=%d, crashed_tid=%d, num_threads=%zu",
        ctx->crashed_pid, ctx->crashed_tid, ctx->platform.num_threads);

    minidump_writer_t writer = { 0 };
    writer.crash_ctx = ctx;

    // Open output file. O_RDWR (not O_WRONLY) because SMART-mode indirect
    // memory capture pread()'s thread-stack bytes back from the dump after
    // they're written, so it can scan them for heap pointers.
    writer.fd = open(output_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (writer.fd < 0) {
        SENTRY_WARNF("failed to create minidump: %s", strerror(errno));
        return -1;
    }

    // Parse process information
    if (parse_proc_maps(&writer) < 0 || enumerate_threads(&writer) < 0) {
        close(writer.fd);
        unlink(output_path);
        return -1;
    }

    // Attach to crashed process via ptrace early so we can:
    // 1. Read memory using ptrace for memory list stream
    // 2. Get FPU state for crashed thread via PTRACE_GETFPREGS
    // 3. Get registers for threads with missing context via PTRACE_GETREGS
    if (!ptrace_attach_process(&writer)) {
        SENTRY_WARN(
            "Failed to attach to process via ptrace, continuing without "
            "it");
        // Continue anyway - we can still write minidump without ptrace
    }

    // Reserve space for header and directory.
    //
    // Streams written (matches the Breakpad linux minidump_writer set so
    // LLDB's ProcessMinidump can identify modules and unwind):
    //   0  SystemInfo
    //   1  ThreadList
    //   2  ModuleList
    //   3  Exception
    //   4  MemoryList
    //   5  LinuxProcStatus     /proc/<pid>/status
    //   6  LinuxMaps           /proc/<pid>/maps
    //   7  ThreadNames
    //   8  LinuxAuxv           /proc/<pid>/auxv         (LLDB needs this)
    //   9  LinuxCpuInfo        /proc/cpuinfo
    //  10  LinuxLsbRelease     /etc/lsb-release
    //  11  LinuxCmdLine        /proc/<pid>/cmdline
    //  12  LinuxEnviron        /proc/<pid>/environ
    //  13  LinuxDsoDebug       walked from r_debug      (LLDB needs this)
    //
    // The optional /proc files (lsb-release missing on many distros, environ
    // unreadable in some sandboxed configs) are wired with
    // NullifyDirectoryEntry semantics: the slot is reserved, but if the writer
    // fails the directory entry stays at type/size/rva = 0 (consumers ignore
    // it).
    const uint32_t stream_count = 14;
    writer.current_offset = sizeof(minidump_header_t)
        + (stream_count * sizeof(minidump_directory_t));

    SENTRY_DEBUGF("reserving space for %u streams, offset=%zu", stream_count,
        writer.current_offset);

    if (lseek(writer.fd, writer.current_offset, SEEK_SET) < 0) {
        SENTRY_WARN("lseek failed");
        if (writer.ptrace_attached) {
            ptrace(PTRACE_DETACH, ctx->crashed_tid, NULL, NULL);
        }
        close(writer.fd);
        unlink(output_path);
        return -1;
    }

    // Write streams
    minidump_directory_t directories[14];
    memset(directories, 0, sizeof(directories));
    int result = 0;

    SENTRY_DEBUG("writing system info stream");
    result |= write_system_info_stream(&writer, &directories[0]);
    SENTRY_DEBUG("writing thread list stream");
    result |= write_thread_list_stream(&writer, &directories[1]);
    SENTRY_DEBUG("writing module list stream");
    result |= write_module_list_stream(&writer, &directories[2]);
    SENTRY_DEBUG("writing exception stream");
    result |= write_exception_stream(&writer, &directories[3]);

    // Write memory list stream (empty for STACK_ONLY, populated for SMART/FULL)
    result |= write_memory_list_stream(&writer, &directories[4]);

    // Write Linux-specific streams for debugger compatibility.
    // These are non-fatal: if they fail, the minidump is still valid. The
    // fallback explicitly zeros data_size and rva (in addition to the type)
    // so a partially-written entry can't end up with data_size > 0 and
    // rva = 0 — which would point parsers at the minidump header.
#    define NULLIFY_DIR_ENTRY(idx, type)                                       \
        do {                                                                   \
            directories[(idx)].stream_type = (type);                           \
            directories[(idx)].data_size = 0;                                  \
            directories[(idx)].rva = 0;                                        \
        } while (0)

    SENTRY_DEBUG("writing linux proc status stream");
    if (write_linux_proc_status_stream(&writer, &directories[5]) < 0) {
        SENTRY_WARN("failed to write linux proc status stream");
        NULLIFY_DIR_ENTRY(5, MINIDUMP_STREAM_LINUX_PROC_STATUS);
    }

    SENTRY_DEBUG("writing linux maps stream");
    if (write_linux_maps_stream(&writer, &directories[6]) < 0) {
        SENTRY_WARN("failed to write linux maps stream");
        NULLIFY_DIR_ENTRY(6, MINIDUMP_STREAM_LINUX_MAPS);
    }

    // Write thread names stream (matches Crashpad format)
    SENTRY_DEBUG("writing thread names stream");
    if (write_thread_names_stream(&writer, &directories[7]) < 0) {
        SENTRY_WARN("failed to write thread names stream");
        NULLIFY_DIR_ENTRY(7, MINIDUMP_STREAM_THREAD_NAMES);
    }

    SENTRY_DEBUG("writing linux auxv stream");
    if (write_linux_auxv_stream(&writer, &directories[8]) < 0) {
        SENTRY_WARN("failed to write linux auxv stream");
        NULLIFY_DIR_ENTRY(8, MINIDUMP_STREAM_LINUX_AUXV);
    }

    SENTRY_DEBUG("writing linux cpu info stream");
    if (write_linux_cpu_info_stream(&writer, &directories[9]) < 0) {
        SENTRY_DEBUG("linux cpu info stream unavailable");
        NULLIFY_DIR_ENTRY(9, MINIDUMP_STREAM_LINUX_CPU_INFO);
    }

    SENTRY_DEBUG("writing linux lsb release stream");
    if (write_linux_lsb_release_stream(&writer, &directories[10]) < 0) {
        // Many distros don't ship /etc/lsb-release; this is expected.
        SENTRY_DEBUG("linux lsb release stream unavailable");
        NULLIFY_DIR_ENTRY(10, MINIDUMP_STREAM_LINUX_LSB_RELEASE);
    }

    SENTRY_DEBUG("writing linux cmd line stream");
    if (write_linux_cmd_line_stream(&writer, &directories[11]) < 0) {
        SENTRY_DEBUG("linux cmd line stream unavailable");
        NULLIFY_DIR_ENTRY(11, MINIDUMP_STREAM_LINUX_CMD_LINE);
    }

    SENTRY_DEBUG("writing linux environ stream");
    if (write_linux_environ_stream(&writer, &directories[12]) < 0) {
        SENTRY_DEBUG("linux environ stream unavailable");
        NULLIFY_DIR_ENTRY(12, MINIDUMP_STREAM_LINUX_ENVIRON);
    }

    SENTRY_DEBUG("writing linux dso debug stream");
    if (write_linux_dso_debug_stream(&writer, &directories[13]) < 0) {
        SENTRY_WARN("failed to write linux dso debug stream");
        NULLIFY_DIR_ENTRY(13, MINIDUMP_STREAM_LINUX_DSO_DEBUG);
    }
#    undef NULLIFY_DIR_ENTRY

    if (result < 0) {
        if (writer.ptrace_attached) {
            ptrace(PTRACE_DETACH, ctx->crashed_tid, NULL, NULL);
        }
        close(writer.fd);
        unlink(output_path);
        return -1;
    }

    // Write header and directory at the beginning
    if (lseek(writer.fd, 0, SEEK_SET) < 0) {
        if (writer.ptrace_attached) {
            ptrace(PTRACE_DETACH, ctx->crashed_tid, NULL, NULL);
        }
        close(writer.fd);
        unlink(output_path);
        return -1;
    }

    if (write_header(&writer, stream_count) < 0) {
        if (writer.ptrace_attached) {
            ptrace(PTRACE_DETACH, ctx->crashed_tid, NULL, NULL);
        }
        close(writer.fd);
        unlink(output_path);
        return -1;
    }

    // Write only the directory entries we actually used
    size_t dir_size = stream_count * sizeof(minidump_directory_t);
    if (write(writer.fd, directories, dir_size) != (ssize_t)dir_size) {
        if (writer.ptrace_attached) {
            ptrace(PTRACE_DETACH, ctx->crashed_tid, NULL, NULL);
        }
        close(writer.fd);
        unlink(output_path);
        return -1;
    }

    close(writer.fd);

    // Detach from process if we attached
    if (writer.ptrace_attached) {
        ptrace(PTRACE_DETACH, ctx->crashed_tid, NULL, NULL);
        SENTRY_DEBUGF("Detached from thread %d", ctx->crashed_tid);
    }

    SENTRY_DEBUG("successfully wrote minidump");
    return 0;
}

#endif // SENTRY_PLATFORM_LINUX || SENTRY_PLATFORM_ANDROID
