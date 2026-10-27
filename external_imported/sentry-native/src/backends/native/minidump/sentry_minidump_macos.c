#include "sentry_boot.h"
#include "sentry_utils.h"

#if defined(SENTRY_PLATFORM_MACOS)

#    include <errno.h>
#    include <fcntl.h>
#    include <mach-o/dyld.h>
#    include <mach-o/loader.h>
#    include <mach/mach.h>
#    include <mach/mach_vm.h>
#    include <pthread.h>
#    include <signal.h>
#    include <stdbool.h>
#    include <stdio.h>
#    include <string.h>
#    include <sys/stat.h>
#    include <sys/sysctl.h>
#    include <time.h>
#    include <unistd.h>

#    include "sentry_alloc.h"
#    include "sentry_logger.h"
#    include "sentry_minidump_common.h"
#    include "sentry_minidump_format.h"
#    include "sentry_minidump_indirect.h"
#    include "sentry_minidump_writer.h"

// Use shared constants from crash context
#    include "../sentry_crash_context.h"

// CodeView record format for storing UUID
// CV signature: 'RSDS' for PDB 7.0 format (we use it for Mach-O UUID too)
#    define CV_SIGNATURE_RSDS 0x53445352 // "RSDS" in little-endian

typedef struct {
    uint32_t cv_signature; // 'RSDS'
    uint8_t signature[16]; // UUID (matches Mach-O LC_UUID)
    uint32_t age; // Always 0 for Mach-O
    char pdb_file_name[1]; // Module path (variable length)
} __attribute__((packed)) cv_info_pdb70_t;

/**
 * Memory region info
 */
typedef struct {
    mach_vm_address_t address;
    mach_vm_size_t size;
    vm_prot_t protection;
} memory_region_t;

/**
 * Minidump writer context for macOS
 * Note: fd and current_offset must be first to match minidump_writer_base_t
 */
typedef struct {
    // Base fields (must match minidump_writer_base_t layout)
    int fd;
    uint64_t current_offset;

    // macOS-specific fields
    const sentry_crash_context_t *crash_ctx;

    task_t task;
    thread_array_t threads;
    mach_msg_type_number_t thread_count;

    memory_region_t regions[SENTRY_CRASH_MAX_MAPPINGS];
    size_t region_count;

    // Thread-stack memory descriptors recorded as we write the ThreadList
    // stream. We replay them into MemoryListStream so debuggers (notably
    // LLDB's ProcessMinidump) can find stack bytes by virtual address —
    // without this LLDB knows the stack region exists but cannot read its
    // contents and unwinding stops after frame 0. thread_stack_tids[i]
    // identifies the thread for the SMART-mode indirect-memory walker so
    // it can find the matching thread context for register scanning.
    // Stored as full 64-bit Mach thread IDs (matching
    // sentry_thread_context_darwin_t::tid) so the lookup against
    // platform.threads[j].tid does not silently miss on values that don't
    // fit in MINIDUMP_THREAD::thread_id's uint32_t.
    minidump_memory_descriptor_t thread_stacks[SENTRY_CRASH_MAX_THREADS];
    uint64_t thread_stack_tids[SENTRY_CRASH_MAX_THREADS];
    size_t thread_stack_count;
} minidump_writer_t;

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
 * Read memory from task
 */
static kern_return_t
read_task_memory(
    task_t task, mach_vm_address_t addr, void *buf, mach_vm_size_t size)
{
    mach_vm_size_t bytes_read = 0;
    kern_return_t kr = mach_vm_read_overwrite(
        task, addr, size, (mach_vm_address_t)buf, &bytes_read);
    if (kr == KERN_SUCCESS && bytes_read != size) {
        return KERN_FAILURE;
    }
    return kr;
}

/**
 * Enumerate memory regions
 */
static int
enumerate_memory_regions(minidump_writer_t *writer)
{
    mach_vm_address_t address = 0;
    writer->region_count = 0;

    while (writer->region_count < SENTRY_CRASH_MAX_MAPPINGS) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object_name = MACH_PORT_NULL;

        kern_return_t kr = mach_vm_region(writer->task, &address, &size,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &info_count,
            &object_name);

        if (kr != KERN_SUCCESS) {
            break;
        }

        memory_region_t *region = &writer->regions[writer->region_count++];
        region->address = address;
        region->size = size;
        region->protection = info.protection;

        address += size;
    }

    SENTRY_DEBUGF("found %zu memory regions", writer->region_count);
    return 0;
}

/**
 * Extract UUID from Mach-O file
 * Returns true if UUID found, false otherwise
 */
static bool
extract_macho_uuid(const char *macho_path, uint8_t uuid[16])
{
    int fd = open(macho_path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    // Read Mach-O header
#    if defined(__x86_64__) || defined(__aarch64__)
    struct mach_header_64 header;
#    else
    struct mach_header header;
#    endif

    if (read(fd, &header, sizeof(header)) != sizeof(header)) {
        close(fd);
        return false;
    }

    // Verify Mach-O magic
#    if defined(__x86_64__)
    uint32_t expected_magic = MH_MAGIC_64;
#    elif defined(__aarch64__)
    uint32_t expected_magic = MH_MAGIC_64;
#    else
    uint32_t expected_magic = MH_MAGIC;
#    endif

    // Only accept native-endian Mach-O. Byte-swapped (MH_CIGAM*) binaries
    // can't be loaded on the current platform, so they should never appear
    // for a running process's modules. Accepting them without swapping all
    // header fields would cause corrupt reads.
    if (header.magic != expected_magic) {
        close(fd);
        return false;
    }

    // Read load commands
    size_t commands_size = header.sizeofcmds;
    void *commands_buf = sentry_malloc(commands_size);
    if (!commands_buf) {
        close(fd);
        return false;
    }

    if (read(fd, commands_buf, commands_size) != (ssize_t)commands_size) {
        sentry_free(commands_buf);
        close(fd);
        return false;
    }

    // Search for LC_UUID command
    uint8_t *ptr = (uint8_t *)commands_buf;
    uint8_t *end = ptr + commands_size;
    bool found = false;
    for (uint32_t i = 0; i < header.ncmds && !found; i++) {
        // Bounds check before reading load_command header
        if (ptr + sizeof(struct load_command) > end) {
            break;
        }
        struct load_command *cmd = (struct load_command *)ptr;

        // Validate cmdsize to prevent buffer over-read
        if (cmd->cmdsize < sizeof(struct load_command)
            || ptr + cmd->cmdsize > end) {
            break;
        }

        if (cmd->cmd == LC_UUID) {
            // Ensure we have enough data for uuid_command
            if (ptr + sizeof(struct uuid_command) <= end) {
                struct uuid_command *uuid_cmd = (struct uuid_command *)ptr;
                memcpy(uuid, uuid_cmd->uuid, 16);
                found = true;
            }
            break;
        }

        ptr += cmd->cmdsize;
    }

    sentry_free(commands_buf);
    close(fd);
    return found;
}

/**
 * Write CodeView record with UUID
 */
static minidump_rva_t
write_cv_record(
    minidump_writer_t *writer, const char *module_path, const uint8_t uuid[16])
{
    if (!uuid) {
        return 0;
    }

    // Calculate size: header + path + null terminator
    size_t path_len = strlen(module_path);
    size_t total_size
        = sizeof(cv_info_pdb70_t) + path_len; // +1 already in struct

    cv_info_pdb70_t *cv_record = sentry_malloc(total_size);
    if (!cv_record) {
        return 0;
    }

    cv_record->cv_signature = CV_SIGNATURE_RSDS;
    cv_record->age = 0; // Not used for Mach-O

    // Copy UUID with byte-swap for GUID format.
    // Mach-O LC_UUID is big-endian (RFC 4122), but the RSDS CV record uses
    // Windows GUID layout where the first 3 fields are little-endian.
    memcpy(cv_record->signature, uuid, 16);
    sentry__uuid_swap_guid_bytes(cv_record->signature);

    // Copy module path
    memcpy(cv_record->pdb_file_name, module_path, path_len + 1);

    minidump_rva_t rva = write_data(writer, cv_record, total_size);
    sentry_free(cv_record);
    return rva;
}

/**
 * Write system info stream
 */
static int
write_system_info_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    minidump_system_info_t sysinfo = { 0 };

#    if defined(__x86_64__)
    sysinfo.processor_architecture = MINIDUMP_CPU_X86_64;
#    elif defined(__aarch64__) || defined(__arm64__)
    sysinfo.processor_architecture = MINIDUMP_CPU_ARM64;
#    elif defined(__i386__)
    sysinfo.processor_architecture = MINIDUMP_CPU_X86;
#    elif defined(__arm__)
    sysinfo.processor_architecture = MINIDUMP_CPU_ARM;
#    endif

    sysinfo.platform_id = MINIDUMP_OS_MACOS;

    int mib[2] = { CTL_HW, HW_NCPU };
    int ncpu = 1;
    size_t len = sizeof(ncpu);
    sysctl(mib, 2, &ncpu, &len, NULL, 0);
    sysinfo.number_of_processors = (uint8_t)ncpu;

    // Set processor level and revision (required for proper parsing)
#    if defined(__aarch64__) || defined(__arm64__)
    sysinfo.processor_level = 8; // ARM v8
    sysinfo.processor_revision = 0;
#    elif defined(__x86_64__)
    sysinfo.processor_level = 6; // P6 family
    sysinfo.processor_revision = 0;
#    endif

    // Set required OS and product type
    sysinfo.product_type = 1; // VER_NT_WORKSTATION

    // Get actual macOS version and build string
    char os_version[256];
    char build_version[256] = "";
    len = sizeof(os_version);
    if (sysctlbyname("kern.osproductversion", os_version, &len, NULL, 0) == 0) {
        // Parse version string like "14.0.0"
        int major = 0, minor = 0, patch = 0;
        sscanf(os_version, "%d.%d.%d", &major, &minor, &patch);
        sysinfo.major_version = major;
        sysinfo.minor_version = minor;
        sysinfo.build_number = patch;

        // Get build version for CSD string
        len = sizeof(build_version);
        sysctlbyname("kern.osversion", build_version, &len, NULL, 0);
    } else {
        // Fallback values
        sysinfo.major_version = 14;
        sysinfo.minor_version = 0;
        sysinfo.build_number = 0;
    }

    // Populate CPU information
#    if defined(__x86_64__) || defined(__i386__)
    // For x86/x86_64, we would populate vendor_id, version_information, etc.
    // For now, zero is acceptable
    memset(&sysinfo.cpu.x86_cpu_info, 0, sizeof(sysinfo.cpu.x86_cpu_info));
#    else
    // For ARM/ARM64 and other architectures, use processor_features
    // These are typically obtained from sysctl or cpuid-like mechanisms
    // For now, zero is acceptable (indicates no special features reported)
    memset(&sysinfo.cpu.other_cpu_info, 0, sizeof(sysinfo.cpu.other_cpu_info));
#    endif

    // Write CSD version string (required by Sentry)
    // Even if empty, must be present
    sysinfo.csd_version_rva
        = write_minidump_string(writer, build_version[0] ? build_version : "");
    if (!sysinfo.csd_version_rva) {
        return -1;
    }

    dir->stream_type = MINIDUMP_STREAM_SYSTEM_INFO;
    dir->rva = write_data(writer, &sysinfo, sizeof(sysinfo));
    dir->data_size = sizeof(sysinfo);

    return dir->rva ? 0 : -1;
}

/**
 * Convert macOS thread state to minidump context
 */
static minidump_rva_t
write_thread_context(
    minidump_writer_t *writer, const _STRUCT_MCONTEXT *mcontext)
{
#    if defined(__x86_64__)
    minidump_context_x86_64_t context = { 0 };
    // Set flags for full context (control + integer + segments + floating
    // point)
    context.context_flags
        = 0x0010003f; // CONTEXT_AMD64 | CONTEXT_CONTROL | CONTEXT_INTEGER |
                      // CONTEXT_SEGMENTS | CONTEXT_FLOATING_POINT

    // Copy general purpose registers
    context.rax = mcontext->__ss.__rax;
    context.rbx = mcontext->__ss.__rbx;
    context.rcx = mcontext->__ss.__rcx;
    context.rdx = mcontext->__ss.__rdx;
    context.rsi = mcontext->__ss.__rsi;
    context.rdi = mcontext->__ss.__rdi;
    context.rbp = mcontext->__ss.__rbp;
    context.rsp = mcontext->__ss.__rsp;
    context.r8 = mcontext->__ss.__r8;
    context.r9 = mcontext->__ss.__r9;
    context.r10 = mcontext->__ss.__r10;
    context.r11 = mcontext->__ss.__r11;
    context.r12 = mcontext->__ss.__r12;
    context.r13 = mcontext->__ss.__r13;
    context.r14 = mcontext->__ss.__r14;
    context.r15 = mcontext->__ss.__r15;
    context.rip = mcontext->__ss.__rip;
    context.eflags = mcontext->__ss.__rflags;
    context.cs = mcontext->__ss.__cs;
    context.fs = mcontext->__ss.__fs;
    context.gs = mcontext->__ss.__gs;

    // Copy FPU state from macOS float state
    context.mx_csr = mcontext->__fs.__fpu_mxcsr;

    // On older macOS, __fpu_fcw and __fpu_fsw are structs, on newer they're
    // uint16_t We need to extract the raw value in both cases
    uint16_t fcw, fsw;
    memcpy(&fcw, &mcontext->__fs.__fpu_fcw, sizeof(uint16_t));
    memcpy(&fsw, &mcontext->__fs.__fpu_fsw, sizeof(uint16_t));

    context.float_save.control_word = fcw;
    context.float_save.status_word = fsw;
    context.float_save.tag_word = mcontext->__fs.__fpu_ftw;
    context.float_save.error_opcode = mcontext->__fs.__fpu_fop;
    context.float_save.error_offset = mcontext->__fs.__fpu_ip;
    context.float_save.error_selector = mcontext->__fs.__fpu_cs;
    context.float_save.data_offset = mcontext->__fs.__fpu_dp;
    context.float_save.data_selector = mcontext->__fs.__fpu_ds;
    context.float_save.mx_csr = mcontext->__fs.__fpu_mxcsr;
    context.float_save.mx_csr_mask = mcontext->__fs.__fpu_mxcsrmask;

    // Copy x87 FPU registers (ST0-ST7)
    for (int i = 0; i < 8; i++) {
        // macOS stores FPU registers as 10-byte values in __fpu_stmm0-7
        // We need to pack them into 128-bit values (only lower 80 bits are
        // valid)
        const uint8_t *fpreg
            = (const uint8_t *)&mcontext->__fs.__fpu_stmm0 + (i * 16);
        memcpy(&context.float_save.float_registers[i], fpreg, 16);
    }

    // Copy XMM registers (XMM0-XMM15)
    for (int i = 0; i < 16; i++) {
        const uint8_t *xmmreg
            = (const uint8_t *)&mcontext->__fs.__fpu_xmm0 + (i * 16);
        memcpy(&context.float_save.xmm_registers[i], xmmreg, 16);
    }

    return write_data(writer, &context, sizeof(context));

#    elif defined(__aarch64__)
    minidump_context_arm64_t context = { 0 };
    // Set flags for control + integer + fpsimd registers (FULL context)
    context.context_flags = 0x00400007; // ARM64 | Control | Integer | Fpsimd

    // Copy general purpose registers X0-X28
    for (int i = 0; i < 29; i++) {
        context.regs[i] = mcontext->__ss.__x[i];
    }
    // Copy FP, LR, SP, PC separately
    context.fp = SENTRY__ARM64_GET_FP(mcontext->__ss); // X29
    context.lr = SENTRY__ARM64_GET_LR(mcontext->__ss); // X30
    context.sp = SENTRY__ARM64_GET_SP(mcontext->__ss);
    context.pc = SENTRY__ARM64_GET_PC(mcontext->__ss);
    context.cpsr = mcontext->__ss.__cpsr;

    // Copy NEON/FP registers (V0-V31)
    memcpy(context.fpsimd, mcontext->__ns.__v, sizeof(mcontext->__ns.__v));
    context.fpsr = mcontext->__ns.__fpsr;
    context.fpcr = mcontext->__ns.__fpcr;

    // Zero out debug registers (not captured)
    memset(context.bcr, 0, sizeof(context.bcr));
    memset(context.bvr, 0, sizeof(context.bvr));
    memset(context.wcr, 0, sizeof(context.wcr));
    memset(context.wvr, 0, sizeof(context.wvr));

    return write_data(writer, &context, sizeof(context));

#    else
#        error "Unsupported architecture"
#    endif
}

/**
 * Push a thread's stack descriptor onto writer->thread_stacks so the
 * MemoryListStream writer can replay it. We only record stacks whose
 * write actually produced bytes — a zero-size or zero-rva descriptor
 * would point into the minidump header and break parsers.
 */
static void
record_thread_stack(
    minidump_writer_t *writer, const minidump_thread_t *thread, uint64_t tid)
{
    if (thread->stack.memory.size == 0 || thread->stack.memory.rva == 0) {
        return;
    }
    if (writer->thread_stack_count >= SENTRY_CRASH_MAX_THREADS) {
        return;
    }
    writer->thread_stacks[writer->thread_stack_count] = thread->stack;
    writer->thread_stack_tids[writer->thread_stack_count] = tid;
    writer->thread_stack_count++;
}

// Forward declaration: `find_region_for_addr` is defined further down
// with the rest of the SMART-mode indirect-capture code, but we want to
// consult it from ``write_thread_stack`` so the read window is clamped
// to the actual VM region containing SP. Without this clamp, a SP near
// the top of a small thread stack causes the fixed-size read to spill
// into the guard page and the entire `read_task_memory` call fails.
static const memory_region_t *find_region_for_addr(
    const minidump_writer_t *writer, uint64_t addr);

/**
 * Read and write stack memory for a thread
 */
static minidump_rva_t
write_thread_stack(minidump_writer_t *writer, uint64_t stack_pointer,
    size_t *stack_size_out, uint64_t *stack_start_out)
{
    // Stack grows downward on macOS (toward lower addresses).
    // SP points to the top of stack (lowest used address).
    // Return addresses and saved registers are at addresses >= SP.
    // We need to capture from SP *upward* for stack unwinding to work.
    const size_t MAX_STACK_SIZE = SENTRY_CRASH_MAX_STACK_CAPTURE;

    // Clamp the read window to the VM region containing SP. Without this,
    // a SP near the top of a small thread stack causes the fixed-size
    // read to overshoot into an unmapped guard page and the entire
    // read_task_memory call fails with KERN_INVALID_ADDRESS, leaving the
    // descriptor empty.
    mach_vm_address_t stack_start = stack_pointer;
    mach_vm_size_t stack_size = MAX_STACK_SIZE;
    const memory_region_t *containing_region
        = find_region_for_addr(writer, stack_pointer);
    if (containing_region) {
        uint64_t region_end
            = containing_region->address + containing_region->size;
        if (stack_pointer < region_end) {
            uint64_t available = region_end - stack_pointer;
            if (available < stack_size) {
                stack_size = (mach_vm_size_t)available;
            }
        }
    }

    // Allocate buffer for stack memory
    void *stack_buffer = sentry_malloc(stack_size);
    if (!stack_buffer) {
        *stack_size_out = 0;
        *stack_start_out = 0;
        return 0;
    }

    // Try to read stack memory from SP upward
    kern_return_t kr
        = read_task_memory(writer->task, stack_start, stack_buffer, stack_size);

    minidump_rva_t rva = 0;
    if (kr == KERN_SUCCESS) {
        rva = write_data(writer, stack_buffer, stack_size);
        *stack_size_out = rva ? stack_size : 0;
        *stack_start_out = rva ? stack_start : 0;
    } else {
        // Defensive retry with progressively smaller reads in case the
        // region query was stale (e.g. region was unmapped between
        // enumerate_memory_regions and now). Halve from the clamped
        // stack_size so we still make at least one attempt when the
        // region is smaller than a page. Floor at 16 bytes (two 64-bit
        // pointers — enough for at least a return address and frame
        // pointer) so very small clamps still get a retry pass.
        for (mach_vm_size_t retry = stack_size / 2; retry >= 16; retry /= 2) {
            kr = read_task_memory(
                writer->task, stack_start, stack_buffer, retry);
            if (kr == KERN_SUCCESS) {
                stack_size = retry;
                rva = write_data(writer, stack_buffer, stack_size);
                *stack_size_out = rva ? stack_size : 0;
                *stack_start_out = rva ? stack_start : 0;
                break;
            }
        }
        if (rva == 0) {
            *stack_size_out = 0;
            *stack_start_out = 0;
        }
    }

    sentry_free(stack_buffer);
    return rva;
}

/**
 * Write thread list stream
 */
static int
write_thread_list_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    uint32_t thread_count = writer->thread_count;

    // In fallback mode (no task_for_pid), use threads from crash context
    if (thread_count == 0 && writer->crash_ctx) {
        if (writer->crash_ctx->platform.num_threads > 0) {
            thread_count = writer->crash_ctx->platform.num_threads;
            // Bounds check to prevent out-of-bounds access on corrupted crash
            // context
            if (thread_count > SENTRY_CRASH_MAX_THREADS) {
                thread_count = SENTRY_CRASH_MAX_THREADS;
            }
            SENTRY_DEBUGF("Using %u threads from crash context", thread_count);
        } else {
            // Last resort: add at least the crashing thread
            thread_count = 1;
            SENTRY_WARN("No threads in crash context, using last resort path");
        }
    } else {
        SENTRY_DEBUGF("Using %u threads from task_threads()", thread_count);
    }

    size_t list_size
        = sizeof(uint32_t) + (thread_count * sizeof(minidump_thread_t));

    minidump_thread_list_t *thread_list = sentry_malloc(list_size);
    if (!thread_list) {
        return -1;
    }

    thread_list->count = thread_count;

    if (writer->thread_count > 0) {
        // Full path: enumerate all threads from task_threads().
        //
        // For *register state* and *stack capture* we MUST prefer the
        // snapshot the signal handler took at crash time (saved into
        // ``writer->crash_ctx->platform.threads[]``) over a fresh
        // ``thread_get_state()`` from inside the daemon. By the time the
        // daemon runs, the crashing thread's signal handler has moved its
        // SP/PC deep into libsystem code (waiting on the daemon via shm
        // IPC), so ``thread_get_state`` returns a daemon-time SP that
        // points inside the daemon-wait frame rather than the crash site.
        // That made every ``MINIDUMP_THREAD::stack.start_of_memory_range``
        // for the crashing thread (and any thread whose SP advanced
        // between signal-time and daemon-time) point at libsystem code
        // pages instead of the real thread stack VM region. Match by
        // ``thread_id`` against the captured snapshot and use that;
        // ``thread_get_state`` only fills the gap if the snapshot is
        // missing the thread for some reason.
        for (mach_msg_type_number_t i = 0; i < writer->thread_count; i++) {
            minidump_thread_t *thread = &thread_list->threads[i];
            memset(thread, 0, sizeof(*thread));

            thread_t mach_thread = writer->threads[i];

            // Get thread ID. Mach's thread_identifier_info.thread_id is
            // uint64_t; MINIDUMP_THREAD::thread_id is uint32_t. Match against
            // the captured snapshot (whose tid is also uint64_t) using the
            // full value, and only narrow when writing the minidump field.
            uint64_t mach_tid = 0;
            thread_identifier_info_data_t identifier_info;
            mach_msg_type_number_t identifier_info_count
                = THREAD_IDENTIFIER_INFO_COUNT;

            if (thread_info(mach_thread, THREAD_IDENTIFIER_INFO,
                    (thread_info_t)&identifier_info, &identifier_info_count)
                == KERN_SUCCESS) {
                mach_tid = identifier_info.thread_id;
                thread->thread_id = (uint32_t)mach_tid;
            }

            // Get thread priority
            thread_extended_info_data_t extended_info;
            mach_msg_type_number_t extended_info_count
                = THREAD_EXTENDED_INFO_COUNT;

            if (thread_info(mach_thread, THREAD_EXTENDED_INFO,
                    (thread_info_t)&extended_info, &extended_info_count)
                == KERN_SUCCESS) {
                thread->priority = extended_info.pth_curpri;
                thread->priority_class = extended_info.pth_priority;
            }

            // Look up the signal-handler-captured snapshot for this thread.
            const _STRUCT_MCONTEXT *snapshot_state = NULL;
            size_t num_captured = writer->crash_ctx
                ? writer->crash_ctx->platform.num_threads
                : 0;
            if (num_captured > SENTRY_CRASH_MAX_THREADS) {
                num_captured = SENTRY_CRASH_MAX_THREADS;
            }
            for (size_t j = 0; j < num_captured; j++) {
                if (writer->crash_ctx->platform.threads[j].tid == mach_tid) {
                    snapshot_state
                        = &writer->crash_ctx->platform.threads[j].state;
                    break;
                }
            }

            // Get thread state (registers): prefer the signal-handler
            // snapshot, fall back to a fresh thread_get_state() only when
            // the snapshot is missing the thread.
            // Zero-initialize so float/NEON state fields are deterministic
            // since MACHINE_THREAD_STATE only populates integer registers
            // (__ss); we must pass &mcontext.__ss (not &mcontext) when
            // calling thread_get_state.
            _STRUCT_MCONTEXT mcontext;
            memset(&mcontext, 0, sizeof(mcontext));
            bool have_state = false;
            if (snapshot_state) {
                memcpy(&mcontext, snapshot_state, sizeof(mcontext));
                have_state = true;
            } else {
                mach_msg_type_number_t state_count = MACHINE_THREAD_STATE_COUNT;
                if (thread_get_state(mach_thread, MACHINE_THREAD_STATE,
                        (thread_state_t)&mcontext.__ss, &state_count)
                    == KERN_SUCCESS) {
                    have_state = true;
                }
            }

            if (have_state) {

                // Write thread context (registers)
                thread->thread_context.rva
                    = write_thread_context(writer, &mcontext);
                thread->thread_context.size
                    = thread->thread_context.rva ? get_context_size() : 0;

                // Write stack memory
                uint64_t sp;
#    if defined(__x86_64__)
                sp = mcontext.__ss.__rsp;
#    elif defined(__aarch64__)
                sp = SENTRY__ARM64_GET_SP(mcontext.__ss);
#    endif
                size_t stack_size = 0;
                uint64_t stack_start = 0;
                thread->stack.memory.rva
                    = write_thread_stack(writer, sp, &stack_size, &stack_start);
                thread->stack.memory.size = stack_size;
                thread->stack.start_address = stack_start;
                record_thread_stack(writer, thread, mach_tid);
            }
        }
    } else if (writer->crash_ctx
        && writer->crash_ctx->platform.num_threads > 0) {
        // Fallback path: use threads captured in signal handler
        for (size_t i = 0;
            i < writer->crash_ctx->platform.num_threads && i < thread_count;
            i++) {
            minidump_thread_t *thread = &thread_list->threads[i];
            memset(thread, 0, sizeof(*thread));

            // Use thread ID captured in signal handler (portable across
            // processes)
            thread->thread_id = writer->crash_ctx->platform.threads[i].tid;

            // Write thread context (registers)
            const _STRUCT_MCONTEXT *state
                = &writer->crash_ctx->platform.threads[i].state;
            thread->thread_context.rva = write_thread_context(writer, state);
            thread->thread_context.size
                = thread->thread_context.rva ? get_context_size() : 0;
            SENTRY_DEBUGF("Thread %zu: wrote context at RVA 0x%x", i,
                thread->thread_context.rva);

            // Write stack memory from file (captured in signal handler)
            uint64_t sp;
#    if defined(__x86_64__)
            sp = state->__ss.__rsp;
#    elif defined(__aarch64__)
            sp = SENTRY__ARM64_GET_SP(state->__ss);
#    endif

            const char *stack_path
                = writer->crash_ctx->platform.threads[i].stack_path;
            uint64_t saved_stack_size
                = writer->crash_ctx->platform.threads[i].stack_size;

            if (stack_path[0] != '\0' && saved_stack_size > 0) {
                // Read stack from file
                int stack_fd = open(stack_path, O_RDONLY);
                if (stack_fd >= 0) {
                    void *stack_buffer = sentry_malloc(saved_stack_size);
                    if (stack_buffer) {
                        ssize_t bytes_read
                            = read(stack_fd, stack_buffer, saved_stack_size);
                        if (bytes_read == (ssize_t)saved_stack_size) {
                            thread->stack.memory.rva = write_data(
                                writer, stack_buffer, saved_stack_size);
                            thread->stack.memory.size = thread->stack.memory.rva
                                ? saved_stack_size
                                : 0;
                            thread->stack.start_address
                                = thread->stack.memory.rva ? sp : 0;
                            record_thread_stack(writer, thread,
                                writer->crash_ctx->platform.threads[i].tid);
                            SENTRY_DEBUGF(
                                "Thread %zu: wrote stack from file at RVA "
                                "0x%x, size %llu, start_addr 0x%llx",
                                i, thread->stack.memory.rva,
                                (unsigned long long)saved_stack_size,
                                (unsigned long long)sp);
                        } else {
                            SENTRY_WARN("Failed to read stack file");
                            thread->stack.memory.rva = 0;
                            thread->stack.memory.size = 0;
                        }
                        sentry_free(stack_buffer);
                    }
                    close(stack_fd);
                    // Note: Don't delete stack file here - the native
                    // stacktrace builder also needs it. It will be cleaned up
                    // when the run folder is deleted.
                } else {
                    SENTRY_WARNF("Failed to open stack file: %s", stack_path);
                    thread->stack.memory.rva = 0;
                    thread->stack.memory.size = 0;
                }
            } else {
                // No saved stack, try to read from memory (will likely fail
                // without task port)
                size_t stack_size = 0;
                uint64_t stack_start = 0;
                thread->stack.memory.rva
                    = write_thread_stack(writer, sp, &stack_size, &stack_start);
                thread->stack.memory.size = stack_size;
                thread->stack.start_address = stack_start;
                record_thread_stack(
                    writer, thread, writer->crash_ctx->platform.threads[i].tid);
                SENTRY_DEBUGF(
                    "Thread %zu: wrote stack from memory at RVA 0x%x, size %zu",
                    i, thread->stack.memory.rva, stack_size);
            }
        }
    } else if (writer->crash_ctx) {
        // Last resort: add just the crashing thread ID
        minidump_thread_t *thread = &thread_list->threads[0];
        memset(thread, 0, sizeof(*thread));
        thread->thread_id = writer->crash_ctx->crashed_tid;
    }

    dir->stream_type = MINIDUMP_STREAM_THREAD_LIST;
    dir->rva = write_data(writer, thread_list, list_size);
    dir->data_size = list_size;

    sentry_free(thread_list);
    return dir->rva ? 0 : -1;
}

/**
 * Write thread names stream (stream type 24).
 *
 * Matches Crashpad's ThreadNamesStream format: a list of MINIDUMP_THREAD_NAME
 * entries, each pointing to a UTF-16LE string. Mirrors the Linux writer
 * (sentry_minidump_linux.c:write_thread_names_stream).
 *
 * Names are read via thread_info(THREAD_EXTENDED_INFO), which returns the
 * `pth_name[64]` field set by pthread_setname_np(). This works cross-process,
 * so we can use it from the daemon as long as we have a valid task port.
 *
 * Fallback path (no task_for_pid): the signal-handler-captured snapshot in
 * ``ctx->platform.threads[]`` does not currently include names, so those
 * entries get an empty string — but the stream is still emitted with the
 * correct tid list so consumers see a non-empty thread_names list.
 */
static int
write_thread_names_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    // Decide which thread set we're iterating, matching
    // write_thread_list_stream.
    uint32_t thread_count = writer->thread_count;
    bool use_fallback = false;

    if (thread_count == 0 && writer->crash_ctx) {
        if (writer->crash_ctx->platform.num_threads > 0) {
            thread_count = writer->crash_ctx->platform.num_threads;
            if (thread_count > SENTRY_CRASH_MAX_THREADS) {
                thread_count = SENTRY_CRASH_MAX_THREADS;
            }
            use_fallback = true;
        } else {
            // Last-resort: only the crashing thread, no name available.
            thread_count = 1;
            use_fallback = true;
        }
    }

    if (thread_count == 0) {
        // Nothing to write — leave dir zeroed so consumers skip the slot.
        return 0;
    }

    SENTRY_DEBUGF("write_thread_names_stream: %u threads", thread_count);

    minidump_rva_t *name_rvas
        = sentry_malloc(sizeof(minidump_rva_t) * thread_count);
    uint32_t *name_tids = sentry_malloc(sizeof(uint32_t) * thread_count);
    if (!name_rvas || !name_tids) {
        sentry_free(name_rvas);
        sentry_free(name_tids);
        return -1;
    }

    // First pass: collect (tid, name) for each thread and write each name as a
    // UTF-16LE MINIDUMP_STRING; record the RVAs. A 0 RVA from
    // write_minidump_string means the string write failed; emitting it into
    // MINIDUMP_THREAD_NAME::thread_name_rva would point parsers at the
    // header, so we abort the whole stream instead. The caller zeroes the
    // directory slot on -1 and keeps the rest of the dump.
    if (!use_fallback) {
        for (mach_msg_type_number_t i = 0; i < writer->thread_count; i++) {
            thread_t mach_thread = writer->threads[i];

            uint32_t tid = 0;
            thread_identifier_info_data_t identifier_info;
            mach_msg_type_number_t identifier_info_count
                = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(mach_thread, THREAD_IDENTIFIER_INFO,
                    (thread_info_t)&identifier_info, &identifier_info_count)
                == KERN_SUCCESS) {
                tid = (uint32_t)identifier_info.thread_id;
            }
            name_tids[i] = tid;

            const char *name = "";
            thread_extended_info_data_t extended_info;
            mach_msg_type_number_t extended_info_count
                = THREAD_EXTENDED_INFO_COUNT;
            if (thread_info(mach_thread, THREAD_EXTENDED_INFO,
                    (thread_info_t)&extended_info, &extended_info_count)
                == KERN_SUCCESS) {
                // pth_name is a fixed-size buffer; ensure NUL-termination.
                extended_info.pth_name[sizeof(extended_info.pth_name) - 1]
                    = '\0';
                name = extended_info.pth_name;
            }
            name_rvas[i] = write_minidump_string(writer, name);
            if (!name_rvas[i]) {
                sentry_free(name_rvas);
                sentry_free(name_tids);
                return -1;
            }
        }
    } else {
        size_t num_captured
            = writer->crash_ctx ? writer->crash_ctx->platform.num_threads : 0;
        if (num_captured > SENTRY_CRASH_MAX_THREADS) {
            num_captured = SENTRY_CRASH_MAX_THREADS;
        }
        for (uint32_t i = 0; i < thread_count; i++) {
            uint32_t tid = 0;
            if (i < num_captured) {
                tid = (uint32_t)writer->crash_ctx->platform.threads[i].tid;
            } else if (writer->crash_ctx) {
                // Last-resort single-thread path.
                tid = writer->crash_ctx->crashed_tid;
            }
            name_tids[i] = tid;
            // No names are captured in the signal handler on macOS today, so
            // emit an empty string. The stream is still useful: consumers can
            // see the tid list, and the absence of names is consistent with
            // "we couldn't task_for_pid and didn't snapshot names".
            name_rvas[i] = write_minidump_string(writer, "");
            if (!name_rvas[i]) {
                sentry_free(name_rvas);
                sentry_free(name_tids);
                return -1;
            }
        }
    }

    // Second pass: write the thread names list structure.
    size_t list_size = sizeof(uint32_t)
        + ((size_t)thread_count * sizeof(minidump_thread_name_t));
    minidump_thread_name_list_t *name_list = sentry_malloc(list_size);
    if (!name_list) {
        sentry_free(name_rvas);
        sentry_free(name_tids);
        return -1;
    }

    name_list->count = thread_count;
    for (uint32_t i = 0; i < thread_count; i++) {
        name_list->thread_names[i].thread_id = name_tids[i];
        name_list->thread_names[i].thread_name_rva = name_rvas[i];
    }

    // Stage rva first so a write_data failure leaves dir untouched
    // (data_size > 0 with rva == 0 would point parsers at the header).
    minidump_rva_t rva = write_data(writer, name_list, list_size);
    sentry_free(name_list);
    sentry_free(name_rvas);
    sentry_free(name_tids);
    if (!rva) {
        return -1;
    }
    dir->stream_type = MINIDUMP_STREAM_THREAD_NAMES;
    dir->rva = rva;
    dir->data_size = list_size;
    return 0;
}

// Apple's BSD ↔ Mach translation uses a set of EXC_SOFTWARE subcodes
// that aren't in the public SDK headers — they live in xnu's
// ``bsd/sys/ux_exception.h``. The reverse-direction mapping (BSD signal
// → Mach exception) used by ``bsd/uxkern/ux_exception.c`` is what every
// minidump tool's ``CrashReason`` decoder expects to see in
// ``exception_record.exception_code`` / ``exception_flags``. The codes
// have been stable since at least 10.5.
//
// Source:
// https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/ux_exception.h
#    define SENTRY_EXC_UNIX_BAD_SYSCALL 0x10000u // raised by SIGSYS
#    define SENTRY_EXC_UNIX_BAD_PIPE 0x10001u // raised by SIGPIPE
#    define SENTRY_EXC_UNIX_ABORT 0x10002u // raised by SIGABRT

/**
 * Translate a BSD (signum, siginfo) pair into the Mach (exception_type,
 * exception_code) pair that Breakpad / Crashpad write into a macOS
 * minidump's MINIDUMP_EXCEPTION fields.
 *
 * The minidump-format convention on macOS (documented in code by
 * Crashpad's ``ExceptionSnapshotMac::Exception()`` →
 * ``MinidumpExceptionWriter`` and Breakpad's
 * ``MinidumpGenerator::WriteExceptionStream``) is:
 *
 *     exception_record.exception_code  = Mach exception type
 *     exception_record.exception_flags = Mach exception subtype
 *     exception_record.exception_information[0..N] = full Mach code vector
 *
 * Crashpad gets these straight from a Mach exception port (the kernel
 * delivers the Mach values directly). Breakpad's primary path does the
 * same. sentry-native's native backend uses BSD signal handlers, so we
 * have to mirror the kernel's BSD↔Mach mapping ourselves: this table
 * matches what xnu's ``ux_exception.c`` does in the *other* direction
 * when it converts a Mach exception into a BSD signal for delivery to
 * userspace; we just run it in reverse. Apple's enum constants are
 * used throughout — no magic numbers or bit shifts.
 *
 * The BSD signal stays available to consumers (lldb, custom analyzers)
 * via ``exception_information[0]`` so the "lldb wants the signal" use
 * case isn't regressed.
 */
static void
bsd_signal_to_mach_exception(
    int signum, const siginfo_t *info, uint32_t *out_type, uint32_t *out_code)
{
    (void)info;
    switch (signum) {
    // xnu's forward mapping (ux_exception.c) for EXC_BAD_ACCESS only emits
    // SIGSEGV when the code is KERN_INVALID_ADDRESS; every other code
    // (KERN_PROTECTION_FAILURE included) becomes SIGBUS. So to round-trip
    // the signal we must pick the code purely from the signal, not from
    // si_code — the SEGV_ACCERR/BUS_OBJERR distinction does not survive
    // xnu's mapping in either direction anyway. Finer-grained fault info
    // is preserved in exception_information[0] (the BSD signal itself).
    case SIGSEGV:
        *out_type = EXC_BAD_ACCESS;
        *out_code = KERN_INVALID_ADDRESS;
        return;
    case SIGBUS:
        *out_type = EXC_BAD_ACCESS;
        *out_code = KERN_PROTECTION_FAILURE;
        return;
    case SIGILL:
        *out_type = EXC_BAD_INSTRUCTION;
        *out_code = 0;
        return;
    case SIGFPE:
        *out_type = EXC_ARITHMETIC;
        *out_code = 0;
        return;
    case SIGTRAP:
        *out_type = EXC_BREAKPOINT;
        *out_code = 0;
        return;
    case SIGABRT:
        *out_type = EXC_SOFTWARE;
        *out_code = SENTRY_EXC_UNIX_ABORT;
        return;
    case SIGSYS:
        *out_type = EXC_SOFTWARE;
        *out_code = SENTRY_EXC_UNIX_BAD_SYSCALL;
        return;
    case SIGPIPE:
        *out_type = EXC_SOFTWARE;
        *out_code = SENTRY_EXC_UNIX_BAD_PIPE;
        return;
    default:
        // Unknown signal — emit EXC_SOFTWARE so the dump still validates
        // and downstream tools can find the BSD signal in
        // exception_information[0].
        *out_type = EXC_SOFTWARE;
        *out_code = (uint32_t)signum;
        return;
    }
}

/**
 * Write exception stream
 */
static int
write_exception_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    minidump_exception_stream_t exception_stream = { 0 };

    exception_stream.thread_id = writer->crash_ctx->crashed_tid;

    // Translate BSD (signum, siginfo) → Mach (exception_type,
    // exception_code) for Breakpad / Crashpad minidump-format
    // compatibility. The BSD signal is preserved in
    // ``exception_information[0]`` so debuggers that look there still
    // get it.
    uint32_t mach_type = 0;
    uint32_t mach_code = 0;
    bsd_signal_to_mach_exception(writer->crash_ctx->platform.signum,
        &writer->crash_ctx->platform.siginfo, &mach_type, &mach_code);
    exception_stream.exception_record.exception_code = mach_type;
    exception_stream.exception_record.exception_flags = mach_code;
    exception_stream.exception_record.exception_address
        = (uint64_t)writer->crash_ctx->platform.siginfo.si_addr;
    // exception_information[0..1] = (BSD signal, faulting address).
    // Crashpad puts the Mach code vector here; for our BSD-signal-derived
    // exception that vector is (mach_code, address) — we surface the BSD
    // signal as #0 instead so consumers (lldb, custom analyzers) that
    // expect a signal there don't break. number_parameters = 2 matches
    // the number of slots actually populated.
    exception_stream.exception_record.number_parameters = 2;
    exception_stream.exception_record.exception_information[0]
        = (uint64_t)writer->crash_ctx->platform.signum;
    exception_stream.exception_record.exception_information[1]
        = (uint64_t)writer->crash_ctx->platform.siginfo.si_addr;

    // Write the crashing thread's context using the dedicated mcontext field
    // (consistent with Linux which uses platform.context)
    const _STRUCT_MCONTEXT *crash_state = &writer->crash_ctx->platform.mcontext;
    exception_stream.thread_context.rva
        = write_thread_context(writer, crash_state);
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
 * Write module list stream (using pre-captured modules from crash context)
 */
static int
write_module_list_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    // Use modules from crash context (captured in signal handler)
    uint32_t module_count = writer->crash_ctx->module_count;

    // Bounds check to prevent out-of-bounds access on corrupted crash context
    if (module_count > SENTRY_CRASH_MAX_MODULES) {
        module_count = SENTRY_CRASH_MAX_MODULES;
    }

    size_t list_size
        = sizeof(uint32_t) + (module_count * sizeof(minidump_module_t));
    minidump_module_list_t *module_list = sentry_malloc(list_size);
    if (!module_list) {
        return -1;
    }

    module_list->count = module_count;

    for (uint32_t i = 0; i < module_count; i++) {
        minidump_module_t *mdmodule = &module_list->modules[i];
        memset(mdmodule, 0, sizeof(*mdmodule));

        const sentry_module_info_t *module = &writer->crash_ctx->modules[i];

        // Set module base address and size
        mdmodule->base_of_image = module->base_address;
        mdmodule->size_of_image = module->size;

        // Set VS_FIXEDFILEINFO signature (first uint32_t of version_info)
        // This is required for minidump processors to recognize the module
        uint32_t version_sig = 0xFEEF04BD;
        memcpy(&mdmodule->version_info[0], &version_sig, sizeof(version_sig));

        // Write module name as UTF-16 string
        mdmodule->module_name_rva = write_minidump_string(writer, module->name);

        // Write CodeView record with UUID for symbolication
        // Try to use UUID captured in signal handler first
        uint8_t uuid[16];
        bool has_uuid = false;

        // Check if UUID was captured in signal handler
        bool uuid_is_zero = true;
        for (int j = 0; j < 16; j++) {
            if (module->uuid[j] != 0) {
                uuid_is_zero = false;
                break;
            }
        }

        if (!uuid_is_zero) {
            // Use UUID from signal handler
            memcpy(uuid, module->uuid, 16);
            has_uuid = true;
        } else {
            // Fallback: Extract UUID from Mach-O file
            has_uuid = extract_macho_uuid(module->name, uuid);
        }

        if (has_uuid) {
            minidump_rva_t cv_rva = write_cv_record(writer, module->name, uuid);
            if (cv_rva) {
                mdmodule->cv_record.rva = cv_rva;
                mdmodule->cv_record.size
                    = sizeof(cv_info_pdb70_t) + strlen(module->name);
            }
        }
    }

    dir->stream_type = MINIDUMP_STREAM_MODULE_LIST;
    dir->rva = write_data(writer, module_list, list_size);
    dir->data_size = list_size;

    sentry_free(module_list);
    return dir->rva ? 0 : -1;
}

/**
 * Write misc info stream (MINIDUMP_MISC_INFO)
 */
static int
write_misc_info_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    // MINIDUMP_MISC_INFO structure
    struct {
        uint32_t size_of_info;
        uint32_t flags1;
        uint32_t process_id;
        uint32_t process_create_time;
        uint32_t process_user_time;
        uint32_t process_kernel_time;
    } __attribute__((packed, aligned(4))) misc_info = { 0 };

    misc_info.size_of_info = sizeof(misc_info);
    misc_info.flags1 = 0x00000001; // MINIDUMP_MISC1_PROCESS_ID
    misc_info.process_id = writer->crash_ctx->crashed_pid;
    misc_info.process_create_time = 0;
    misc_info.process_user_time = 0;
    misc_info.process_kernel_time = 0;

    dir->stream_type = 15; // MiscInfoStream
    dir->rva = write_data(writer, &misc_info, sizeof(misc_info));
    dir->data_size = sizeof(misc_info);

    return dir->rva ? 0 : -1;
}

/**
 * Check if a memory region should be included based on minidump mode
 */
static bool
should_include_region_macos(const memory_region_t *region,
    const sentry_crash_context_t *crash_ctx, uint64_t crash_addr)
{
    sentry_minidump_mode_t mode = crash_ctx->minidump_mode;

    // STACK_ONLY: Don't include heap regions (stack is in thread list)
    if (mode == SENTRY_MINIDUMP_MODE_STACK_ONLY) {
        return false;
    }

    // FULL: Include all readable regions
    if (mode == SENTRY_MINIDUMP_MODE_FULL) {
        return (region->protection & VM_PROT_READ) != 0;
    }

    // SMART: Include only regions containing the crash address, matching the
    // Linux behavior. We do NOT include all rw regions as that captures too
    // much memory (thread stacks, mmap allocations, etc.) and produces
    // minidumps 10-30x larger than expected. Stack memory is already captured
    // per-thread in the thread list stream.
    if (mode == SENTRY_MINIDUMP_MODE_SMART) {
        bool readable = (region->protection & VM_PROT_READ) != 0;
        if (!readable) {
            return false;
        }

        // Include region containing crash address
        if (crash_addr >= region->address
            && crash_addr < region->address + region->size) {
            return true;
        }

        // Include first page of loaded modules (Mach-O headers) for offline
        // symbolication, matching breakpad/crashpad behavior.
        uint32_t mod_count = crash_ctx->module_count;
        if (mod_count > SENTRY_CRASH_MAX_MODULES) {
            mod_count = SENTRY_CRASH_MAX_MODULES;
        }
        for (uint32_t i = 0; i < mod_count; i++) {
            if (region->address == crash_ctx->modules[i].base_address) {
                return true;
            }
        }
    }

    return false;
}

/**
 * Write module header pages from the signal handler's captured file.
 * The file format is: 4096 bytes per module, concatenated in module order.
 * Returns the number of modules successfully written.
 */
static size_t
write_module_headers_from_capture(minidump_writer_t *writer,
    minidump_memory_descriptor_t *ranges, uint32_t mod_count)
{
    const size_t HEADER_PAGE_SIZE = 4096;

    // Build path: {database_path}/__sentry-modheaders
    const char *db_path = writer->crash_ctx->database_path;
    size_t db_len = strlen(db_path);
    char hdr_path[SENTRY_CRASH_MAX_PATH];
    if (db_len + 22 >= sizeof(hdr_path)) {
        return 0;
    }
    snprintf(hdr_path, sizeof(hdr_path), "%s/__sentry-modheaders", db_path);

    int fd = open(hdr_path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    uint8_t buf[4096];
    size_t written_count = 0;

    for (uint32_t i = 0; i < mod_count; i++) {
        ssize_t nread = read(fd, buf, HEADER_PAGE_SIZE);
        minidump_memory_descriptor_t *mem = &ranges[i];

        if (nread > 0) {
            mem->start_address = writer->crash_ctx->modules[i].base_address;
            mem->memory.rva = write_data(writer, buf, (size_t)nread);
            mem->memory.size = mem->memory.rva ? (uint32_t)nread : 0;
            if (mem->memory.size > 0) {
                written_count++;
            }
        } else {
            mem->start_address = writer->crash_ctx->modules[i].base_address;
            mem->memory.size = 0;
            mem->memory.rva = 0;
        }
    }

    close(fd);
    // Clean up the capture file
    unlink(hdr_path);
    return written_count;
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
// This block is the macOS platform shim.

/**
 * Find the VM region containing addr via binary search over writer->regions.
 * mach_vm_region enumerates in ascending address order so the array is sorted.
 */
static const memory_region_t *
find_region_for_addr(const minidump_writer_t *writer, uint64_t addr)
{
    size_t lo = 0;
    size_t hi = writer->region_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const memory_region_t *r = &writer->regions[mid];
        if (addr < r->address) {
            hi = mid;
        } else if (addr >= r->address + r->size) {
            lo = mid + 1;
        } else {
            return r;
        }
    }
    return NULL;
}

static bool
macos_indirect_is_writable_heap(void *ctx, uint64_t addr)
{
    const minidump_writer_t *writer = (const minidump_writer_t *)ctx;
    const memory_region_t *r = find_region_for_addr(writer, addr);
    if (!r) {
        return false;
    }
    // Must be readable+writable, not executable. macOS regions don't carry
    // names, so we can't reject thread stacks here cheaply — but the
    // accumulator's overlap-check against thread stacks already in the
    // memory list keeps duplicate captures bounded.
    if ((r->protection & VM_PROT_READ) == 0) {
        return false;
    }
    if ((r->protection & VM_PROT_WRITE) == 0) {
        return false;
    }
    if ((r->protection & VM_PROT_EXECUTE) != 0) {
        return false;
    }
    return true;
}

static ssize_t
macos_indirect_read_memory(void *ctx, uint64_t addr, void *buf, size_t len)
{
    minidump_writer_t *writer = (minidump_writer_t *)ctx;
    kern_return_t kr = read_task_memory(
        writer->task, (mach_vm_address_t)addr, buf, (mach_vm_size_t)len);
    return kr == KERN_SUCCESS ? (ssize_t)len : -1;
}

/**
 * Walk the crashing thread's GPRs through sentry__indirect_consider. We
 * only have stored mcontexts for threads captured via the signal handler
 * (typically the crashing one); thread_get_state-derived contexts for
 * other threads are not retained, so their registers aren't scanned here.
 * Their stack contents still get walked.
 */
static void
walk_indirect_registers_for_tid(minidump_writer_t *writer, uint64_t tid,
    sentry_indirect_accumulator_t *acc, const sentry_indirect_ops_t *ops)
{
    const _STRUCT_MCONTEXT *state = NULL;
    size_t num = writer->crash_ctx->platform.num_threads;
    if (num > SENTRY_CRASH_MAX_THREADS) {
        num = SENTRY_CRASH_MAX_THREADS;
    }
    for (size_t j = 0; j < num; j++) {
        if (writer->crash_ctx->platform.threads[j].tid == tid) {
            state = &writer->crash_ctx->platform.threads[j].state;
            break;
        }
    }
    if (!state) {
        return;
    }

    minidump_writer_base_t *base = (minidump_writer_base_t *)writer;
#    if defined(__x86_64__)
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__rax, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__rbx, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__rcx, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__rdx, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__rsi, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__rdi, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__rbp, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__rsp, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__r8, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__r9, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__r10, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__r11, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__r12, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__r13, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__r14, ops);
    sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__r15, ops);
#    elif defined(__aarch64__)
    for (int r = 0; r <= 28; r++) {
        sentry__indirect_consider(acc, base, (uint64_t)state->__ss.__x[r], ops);
    }
    sentry__indirect_consider(
        acc, base, (uint64_t)SENTRY__ARM64_GET_FP(state->__ss), ops);
    sentry__indirect_consider(
        acc, base, (uint64_t)SENTRY__ARM64_GET_LR(state->__ss), ops);
    sentry__indirect_consider(
        acc, base, (uint64_t)SENTRY__ARM64_GET_SP(state->__ss), ops);
#    endif
}

/**
 * For SMART mode: scan every captured thread's stack and the crashing
 * thread's registers for heap pointers, capturing a chunk around each.
 * Stack bytes are pread()'d back from disk (they were written earlier
 * when the thread list was emitted) rather than retained in memory.
 */
static void
capture_indirect_memory_macos(
    minidump_writer_t *writer, sentry_indirect_accumulator_t *acc)
{
    sentry_indirect_ops_t ops = {
        .is_writable_heap = macos_indirect_is_writable_heap,
        .read_memory = macos_indirect_read_memory,
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

        walk_indirect_registers_for_tid(
            writer, writer->thread_stack_tids[i], acc, &ops);

        if (acc->total_bytes >= SENTRY_INDIRECT_MAX_TOTAL_BYTES
            || acc->region_count >= SENTRY_INDIRECT_MAX_REGIONS) {
            break;
        }
    }
}

/**
 * Write memory list stream with memory based on minidump mode.
 *
 * When we have a task port (full path), memory is read from the process.
 * When task_for_pid failed (minimal path), module header pages are read
 * directly from the module files on disk so debuggers can still identify
 * modules for offline symbolication.
 */
static int
write_memory_list_stream(minidump_writer_t *writer, minidump_directory_t *dir)
{
    sentry_minidump_mode_t mode = writer->crash_ctx->minidump_mode;
    uint64_t crash_addr = (uint64_t)writer->crash_ctx->platform.siginfo.si_addr;

    // STACK_ONLY: Don't write memory list (stack is in thread list already)
    if (mode == SENTRY_MINIDUMP_MODE_STACK_ONLY) {
        uint32_t count = 0;
        dir->stream_type = MINIDUMP_STREAM_MEMORY_LIST;
        dir->rva = write_data(writer, &count, sizeof(count));
        dir->data_size = sizeof(count);
        return dir->rva ? 0 : -1;
    }

    // When we have VM regions (task_for_pid succeeded), use them
    if (writer->region_count > 0) {
        // SMART mode: scan registers + stack words for pointers into
        // writable heap regions and capture a chunk around each. Has to
        // run before the memory_list allocation below so we know the
        // final region count.
        sentry_indirect_accumulator_t indirect_acc;
        sentry__indirect_init(&indirect_acc);
        if (mode == SENTRY_MINIDUMP_MODE_SMART) {
            capture_indirect_memory_macos(writer, &indirect_acc);
            SENTRY_DEBUGF("indirect memory: captured %zu regions, %zu bytes",
                indirect_acc.region_count, indirect_acc.total_bytes);
        }

        // Count regions to include
        size_t region_count = 0;
        for (size_t i = 0; i < writer->region_count; i++) {
            if (should_include_region_macos(
                    &writer->regions[i], writer->crash_ctx, crash_addr)) {
                region_count++;
            }
        }

        size_t total_count = region_count + writer->thread_stack_count
            + indirect_acc.region_count;

        size_t list_size = sizeof(uint32_t)
            + (total_count * sizeof(minidump_memory_descriptor_t));
        minidump_memory_list_t *memory_list = sentry_malloc(list_size);
        if (!memory_list) {
            goto empty_list;
        }

        memory_list->count = total_count;

        size_t mem_idx = 0;

        // Replay thread stacks first — bytes are already on disk via the
        // thread-list writer, so this just emits descriptors pointing at
        // the same RVA. Lets debuggers resolve memory at stack addresses
        // and walk the FP chain.
        for (size_t s = 0; s < writer->thread_stack_count; s++) {
            memory_list->ranges[mem_idx++] = writer->thread_stacks[s];
        }

        // Append indirectly-referenced regions. Already deduped + capped
        // by the walker.
        for (size_t s = 0; s < indirect_acc.region_count; s++) {
            minidump_memory_descriptor_t *mem = &memory_list->ranges[mem_idx++];
            mem->start_address = indirect_acc.regions[s].start;
            mem->memory.rva = indirect_acc.regions[s].rva;
            mem->memory.size = indirect_acc.regions[s].size;
        }

        for (size_t i = 0; i < writer->region_count && mem_idx < total_count;
            i++) {
            if (!should_include_region_macos(
                    &writer->regions[i], writer->crash_ctx, crash_addr)) {
                continue;
            }

            memory_region_t *region = &writer->regions[i];
            minidump_memory_descriptor_t *mem = &memory_list->ranges[mem_idx++];

            mach_vm_size_t region_size = region->size;

            // For SMART mode, cap module header pages to one page.
            // Skip the cap if this region contains the crash address.
            if (mode == SENTRY_MINIDUMP_MODE_SMART
                && !(crash_addr >= region->address
                    && crash_addr < region->address + region->size)) {
                uint32_t mod_count = writer->crash_ctx->module_count;
                if (mod_count > SENTRY_CRASH_MAX_MODULES) {
                    mod_count = SENTRY_CRASH_MAX_MODULES;
                }
                for (uint32_t j = 0; j < mod_count; j++) {
                    if (region->address
                        == writer->crash_ctx->modules[j].base_address) {
                        if (region_size > 4096) {
                            region_size = 4096;
                        }
                        break;
                    }
                }
            }

            // Cap at 4MB
            if (region_size > 4 * 1024 * 1024) {
                region_size = 4 * 1024 * 1024;
            }

            void *region_buffer = sentry_malloc(region_size);
            if (!region_buffer) {
                mem->start_address = region->address;
                mem->memory.size = 0;
                mem->memory.rva = 0;
                continue;
            }

            kern_return_t kr = read_task_memory(
                writer->task, region->address, region_buffer, region_size);
            if (kr == KERN_SUCCESS) {
                mem->start_address = region->address;
                mem->memory.rva
                    = write_data(writer, region_buffer, region_size);
                mem->memory.size = mem->memory.rva ? region_size : 0;
            } else {
                mem->start_address = region->address;
                mem->memory.size = 0;
                mem->memory.rva = 0;
            }

            sentry_free(region_buffer);
        }

        dir->stream_type = MINIDUMP_STREAM_MEMORY_LIST;
        dir->rva = write_data(writer, memory_list, list_size);
        dir->data_size = list_size;

        sentry_free(memory_list);

        // Clean up the capture file written by the signal handler since
        // we used VM regions instead.
        const char *db_path = writer->crash_ctx->database_path;
        size_t db_len = strlen(db_path);
        char hdr_path[SENTRY_CRASH_MAX_PATH];
        if (db_len + 22 < sizeof(hdr_path)) {
            snprintf(
                hdr_path, sizeof(hdr_path), "%s/__sentry-modheaders", db_path);
            unlink(hdr_path);
        }

        return dir->rva ? 0 : -1;
    }

    // Minimal path: task_for_pid failed, no VM regions available.
    // Read module header pages from the file captured by the signal handler.
    // The signal handler saves the first 4096 bytes of each module's in-memory
    // Mach-O header (including dyld shared cache modules that don't exist as
    // individual files on disk).
    uint32_t mod_count = writer->crash_ctx->module_count;
    if (mod_count > SENTRY_CRASH_MAX_MODULES) {
        mod_count = SENTRY_CRASH_MAX_MODULES;
    }

    size_t list_size
        = sizeof(uint32_t) + (mod_count * sizeof(minidump_memory_descriptor_t));
    minidump_memory_list_t *memory_list = sentry_malloc(list_size);
    if (!memory_list) {
        goto empty_list;
    }
    memset(memory_list, 0, list_size);

    memory_list->count = mod_count;

    write_module_headers_from_capture(writer, memory_list->ranges, mod_count);

    dir->stream_type = MINIDUMP_STREAM_MEMORY_LIST;
    dir->rva = write_data(writer, memory_list, list_size);
    dir->data_size = list_size;

    sentry_free(memory_list);
    return dir->rva ? 0 : -1;

empty_list:;
    uint32_t count = 0;
    dir->stream_type = MINIDUMP_STREAM_MEMORY_LIST;
    dir->rva = write_data(writer, &count, sizeof(count));
    dir->data_size = sizeof(count);
    return dir->rva ? 0 : -1;
}

/**
 * Main minidump writer for macOS
 */
int
sentry__write_minidump(
    const sentry_crash_context_t *ctx, const char *output_path)
{
    // For now, write a minimal but valid minidump with just the crash context
    // Full memory dump would require task_for_pid entitlements

    SENTRY_DEBUG("write_minidump: starting");

    minidump_writer_t writer = { 0 };
    writer.crash_ctx = ctx;

    // Open output file. O_RDWR (not O_WRONLY) because SMART-mode indirect
    // memory capture pread()'s thread-stack bytes back from the dump after
    // they're written, so it can scan them for heap pointers.
    SENTRY_DEBUGF("write_minidump: opening file %s", output_path);
    writer.fd = open(output_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (writer.fd < 0) {
        SENTRY_WARN("write_minidump: failed to open file");
        return -1;
    }
    SENTRY_DEBUGF("write_minidump: file opened, fd=%d", writer.fd);

    // Try to get task port for crashed process (may fail without entitlements)
    SENTRY_DEBUG("write_minidump: getting task port");
    kern_return_t kr
        = task_for_pid(mach_task_self(), ctx->crashed_pid, &writer.task);
    if (kr != KERN_SUCCESS) {
        SENTRY_DEBUGF("write_minidump: task_for_pid failed (%d), writing "
                      "minimal minidump",
            kr);
        // Without task port, write minimal minidump with all required streams
        // Matching Crashpad's minimum: SystemInfo, MiscInfo, ThreadList,
        // Exception, ModuleList, MemoryList. We additionally emit ThreadNames
        // (with empty name strings) so consumers always see the same stream
        // shape as the full path.
        writer.task = MACH_PORT_NULL;
        writer.thread_count = 0;

        // Reserve space for header and directory (7 streams), position file
        // after them
        const uint32_t stream_count = 7;
        writer.current_offset = sizeof(minidump_header_t)
            + stream_count * sizeof(minidump_directory_t);
        SENTRY_DEBUG("write_minidump: seeking to stream offset");
        if (lseek(writer.fd, writer.current_offset, SEEK_SET) < 0) {
            SENTRY_WARN("write_minidump: lseek failed");
            goto fallback_error;
        }

        // Write streams in same order as Crashpad (will update directory RVAs
        // and current_offset)
        minidump_directory_t directories[7] = { 0 };
        SENTRY_DEBUG("write_minidump: writing system_info stream");
        if (write_system_info_stream(&writer, &directories[0]) < 0) {
            SENTRY_WARN("write_minidump: system_info failed");
            goto fallback_error;
        }
        SENTRY_DEBUG("write_minidump: writing misc_info stream");
        if (write_misc_info_stream(&writer, &directories[1]) < 0) {
            SENTRY_WARN("write_minidump: misc_info failed");
            goto fallback_error;
        }
        SENTRY_DEBUG("write_minidump: writing thread_list stream");
        if (write_thread_list_stream(&writer, &directories[2]) < 0) {
            SENTRY_WARN("write_minidump: thread_list failed");
            goto fallback_error;
        }
        SENTRY_DEBUG("write_minidump: writing exception stream");
        if (write_exception_stream(&writer, &directories[3]) < 0) {
            SENTRY_WARN("write_minidump: exception failed");
            goto fallback_error;
        }
        SENTRY_DEBUG("write_minidump: writing module_list stream");
        if (write_module_list_stream(&writer, &directories[4]) < 0) {
            SENTRY_WARN("write_minidump: module_list failed");
            goto fallback_error;
        }
        SENTRY_DEBUG("write_minidump: writing memory_list stream");
        if (write_memory_list_stream(&writer, &directories[5]) < 0) {
            SENTRY_WARN("write_minidump: memory_list failed");
            goto fallback_error;
        }
        SENTRY_DEBUG("write_minidump: writing thread_names stream");
        if (write_thread_names_stream(&writer, &directories[6]) < 0) {
            // Non-fatal: leave the slot zeroed (consumers ignore stream_type=0)
            // rather than failing the whole dump for a cosmetic stream.
            SENTRY_WARN("write_minidump: thread_names failed, continuing");
            directories[6].stream_type = 0;
            directories[6].rva = 0;
            directories[6].data_size = 0;
        }
        SENTRY_DEBUG("write_minidump: all streams written");

        // Now write header and directory at the beginning
        SENTRY_DEBUG("write_minidump: seeking to beginning for header");
        if (lseek(writer.fd, 0, SEEK_SET) < 0) {
            SENTRY_WARN("write_minidump: lseek to beginning failed");
            goto fallback_error;
        }

        SENTRY_DEBUG("write_minidump: writing header");
        minidump_header_t header = { .signature = MINIDUMP_SIGNATURE,
            .version = MINIDUMP_VERSION,
            .stream_count = stream_count,
            .stream_directory_rva = sizeof(minidump_header_t),
            .checksum = 0,
            .time_date_stamp = (uint32_t)time(NULL),
            .flags = 0 };
        if (write(writer.fd, &header, sizeof(header)) != sizeof(header)) {
            SENTRY_WARN("write_minidump: header write failed");
            goto fallback_error;
        }

        SENTRY_DEBUG("write_minidump: writing directory");
        // Write directory
        if (write(writer.fd, directories, sizeof(directories))
            != sizeof(directories)) {
            SENTRY_WARN("write_minidump: directory write failed");
            goto fallback_error;
        }

        SENTRY_DEBUG("write_minidump: closing file");
        close(writer.fd);
        SENTRY_DEBUG("write_minidump: success");
        return 0;

    fallback_error:
        close(writer.fd);
        unlink(output_path);
        return -1;
    }

    // Get threads
    kr = task_threads(writer.task, &writer.threads, &writer.thread_count);
    if (kr != KERN_SUCCESS) {
        SENTRY_WARNF("failed to get threads: %d", kr);
        mach_port_deallocate(mach_task_self(), writer.task);
        close(writer.fd);
        unlink(output_path);
        return -1;
    }

    // Enumerate memory regions
    enumerate_memory_regions(&writer);

    // Reserve space for header and directory
    // Write 7 streams: system_info, misc_info, threads, exception,
    // module_list, memory_list, thread_names
    const uint32_t stream_count = 7;
    writer.current_offset = sizeof(minidump_header_t)
        + (stream_count * sizeof(minidump_directory_t));

    if (lseek(writer.fd, writer.current_offset, SEEK_SET) < 0) {
        goto cleanup_error;
    }

    // Write streams. ThreadNames is treated as non-fatal: if it fails, the
    // slot stays zeroed and the rest of the dump is still valid.
    minidump_directory_t directories[7] = { 0 };
    int result = 0;

    result |= write_system_info_stream(&writer, &directories[0]);
    result |= write_misc_info_stream(&writer, &directories[1]);
    result |= write_thread_list_stream(&writer, &directories[2]);
    result |= write_exception_stream(&writer, &directories[3]);
    result |= write_module_list_stream(&writer, &directories[4]);
    result |= write_memory_list_stream(&writer, &directories[5]);

    if (result < 0) {
        goto cleanup_error;
    }

    if (write_thread_names_stream(&writer, &directories[6]) < 0) {
        SENTRY_WARN("write_minidump: thread_names failed, continuing");
        directories[6].stream_type = 0;
        directories[6].rva = 0;
        directories[6].data_size = 0;
    }

    // Write header and directory
    if (lseek(writer.fd, 0, SEEK_SET) < 0) {
        goto cleanup_error;
    }

    if (write_header(&writer, stream_count) < 0) {
        goto cleanup_error;
    }

    if (write(writer.fd, directories, sizeof(directories))
        != sizeof(directories)) {
        goto cleanup_error;
    }

    // Cleanup - success path
    for (mach_msg_type_number_t i = 0; i < writer.thread_count; i++) {
        mach_port_deallocate(mach_task_self(), writer.threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)writer.threads,
        writer.thread_count * sizeof(thread_t));
    mach_port_deallocate(mach_task_self(), writer.task);

    close(writer.fd);

    SENTRY_DEBUG("successfully wrote minidump");
    return 0;

cleanup_error:
    // Cleanup - error path (same as success, but delete file and return error)
    for (mach_msg_type_number_t i = 0; i < writer.thread_count; i++) {
        mach_port_deallocate(mach_task_self(), writer.threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)writer.threads,
        writer.thread_count * sizeof(thread_t));
    mach_port_deallocate(mach_task_self(), writer.task);
    close(writer.fd);
    unlink(output_path);
    return -1;
}

#endif // SENTRY_PLATFORM_MACOS
