#include "sentry_boot.h"
#include "sentry_logger.h"
#include "sentry_unwinder.h"
#include <libunwind.h>

#if defined(SENTRY_PLATFORM_MACOS)
#    include <mach/mach.h>
#    include <mach/mach_vm.h>
#endif

// On arm64(e), return addresses may have Pointer Authentication Code (PAC) bits
// set in the upper bits. These must be stripped before symbolization.
// The mask 0x7fffffffffff keeps the lower 47 bits which is the actual address.
// See:
// https://developer.apple.com/documentation/security/preparing_your_app_to_work_with_pointer_authentication
#if defined(__arm64__)
#    define STRIP_PAC(addr) ((addr) & 0x7fffffffffffull)
#else
#    define STRIP_PAC(addr) (addr)
#endif

#if defined(SENTRY_PLATFORM_MACOS)
// Basic pointer validation to make sure we stay inside mapped memory.
// `cache` is an optional pointer to a previously-validated VM region.  When
// the requested range falls inside the cached region we skip the
// mach_vm_region kernel trap entirely. This eliminates nearly all the traps
// during a typical frame-pointer walk because adjacent frames almost always
// live in the same stack region.
static bool
is_readable_ptr(uintptr_t p, size_t size, mem_range_t *cache)
{
    if (!p) {
        return false;
    }

    uintptr_t end = p + (uintptr_t)size;
    if (end < p) {
        return false;
    }

    // Fast path: check against the cached region.
    if (cache && cache->lo < cache->hi && p >= cache->lo && end <= cache->hi) {
        return true;
    }

    // Slow path: ask the kernel.
    mach_vm_address_t address = (mach_vm_address_t)p;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;

    kern_return_t kr = mach_vm_region(mach_task_self(), &address, &region_size,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), object);
    }
    if (kr != KERN_SUCCESS) {
        return false;
    }

    if (!(info.protection & VM_PROT_READ)) {
        return false;
    }

    uintptr_t region_lo = address;
    uintptr_t region_hi = (uintptr_t)address + (uintptr_t)region_size;
    if (region_hi < region_lo) {
        return false;
    }

    if (p < region_lo || end > region_hi) {
        return false;
    }

    // Update the cache so subsequent checks in the same region are free.
    if (cache) {
        cache->lo = region_lo;
        cache->hi = region_hi;
    }

    return true;
}
#endif

static bool
valid_ptr(uintptr_t p, mem_range_t *cache)
{
    if (!p || (p % sizeof(uintptr_t)) != 0) {
        return false;
    }

#if defined(SENTRY_PLATFORM_MACOS)
    return is_readable_ptr(p, sizeof(uintptr_t) * 2, cache);
#else
    (void)cache;
    return true;
#endif
}

/**
 * This does the same frame-pointer walk for arm64 and x86_64, with the only
 * difference being which registers value is used as frame-pointer (fp vs rbp)
 */
static void
fp_walk(
    uintptr_t fp, size_t *n, void **ptrs, size_t max_frames, mem_range_t *cache)
{
    while (*n < max_frames) {
        if (!valid_ptr(fp, cache)) {
            break;
        }

        // arm64 frame record layout: [prev_fp, saved_lr] at fp and fp+8
        // x86_64 frame record layout: [prev_rbp, saved_retaddr] at bp and bp+8
        const uintptr_t *record = (uintptr_t *)fp;
        const uintptr_t next_fp = record[0];
        uintptr_t ret_addr = record[1];
        if (!valid_ptr(next_fp, cache) || !ret_addr) {
            break;
        }

        ret_addr = STRIP_PAC(ret_addr);
        ptrs[(*n)++] = (void *)(ret_addr - 1);
        if (next_fp <= fp) {
            break; // prevent loops
        }
        fp = next_fp;
    }
}

static size_t
fp_walk_from_uctx(const sentry_ucontext_t *uctx, void **ptrs, size_t max_frames)
{
    size_t n = 0;
    mem_range_t cache = { 0, 0 };
    struct __darwin_mcontext64 *mctx = uctx->user_context->uc_mcontext;
#if defined(__arm64__)
    uintptr_t pc, fp, lr;

#    if defined(__arm64e__)
    // arm64e uses opaque accessors that handle PAC authentication
    pc = __darwin_arm_thread_state64_get_pc(mctx->__ss);
    fp = __darwin_arm_thread_state64_get_fp(mctx->__ss);
    lr = __darwin_arm_thread_state64_get_lr(mctx->__ss);
#    else
    // arm64 can access members directly, strip PAC defensively
    pc = STRIP_PAC((uintptr_t)mctx->__ss.__pc);
    fp = (uintptr_t)mctx->__ss.__fp;
    lr = STRIP_PAC((uintptr_t)mctx->__ss.__lr);
#    endif

    // top frame: crash PC points to the faulting instruction, no adjustment
    if (pc && n < max_frames) {
        ptrs[n++] = (void *)pc;
    }

    // Emit LR as the next frame (return address, so adjust -1).
    if (lr && n < max_frames) {
        ptrs[n++] = (void *)(lr - 1);
    }

    // Determine where to start the frame pointer walk:
    // If LR matches the saved LR in the current frame record,
    // the crashing function has a frame and hasn't made sub-calls since its
    // prologue
    //   -> walking from fp would produce a duplicate.
    //   -> Skip to the caller's frame pointer.
    //
    // When they differ,
    // either the function has no frame record (fp belongs to the caller, and LR
    // is the only reference to the caller frame)
    // or
    // it sub-calls (LR is a stale return within the crashing function).
    //
    // In both cases, walking from fp captures the correct remaining frames.
    if (valid_ptr(fp, &cache)) {
        const uintptr_t *record = (uintptr_t *)fp;
        uintptr_t saved_lr = STRIP_PAC(record[1]);
        if (lr == saved_lr) {
            fp_walk(record[0], &n, ptrs, max_frames, &cache);
        } else {
            fp_walk(fp, &n, ptrs, max_frames, &cache);
        }
    }
#elif defined(__x86_64__)
    uintptr_t ip = (uintptr_t)mctx->__ss.__rip;
    uintptr_t bp = (uintptr_t)mctx->__ss.__rbp;

    // top frame: no adjustment
    if (ip && n < max_frames) {
        ptrs[n++] = (void *)ip;
    }

    fp_walk(bp, &n, ptrs, max_frames, &cache);
#else
#    error "Unsupported CPU architecture for macOS stackwalker"
#endif
    return n;
}

size_t
sentry__unwind_stack_libunwind_mac(
    void *addr, const sentry_ucontext_t *uctx, void **ptrs, size_t max_frames)
{
    if (addr) {
        size_t n = 0;
        mem_range_t cache = { 0, 0 };
        fp_walk((uintptr_t)addr, &n, ptrs, max_frames, &cache);
        return n;
    }

    if (uctx) {
        return fp_walk_from_uctx(uctx, ptrs, max_frames);
    }

    // fall back on the system `libunwind` for stack-traces "from call-site"
    unw_context_t uc;
    int ret = unw_getcontext(&uc);
    if (ret != 0) {
        SENTRY_WARN("Failed to retrieve context with libunwind");
        return 0;
    }

    unw_cursor_t cursor;
    ret = unw_init_local(&cursor, &uc);
    if (ret != 0) {
        SENTRY_WARN("Failed to initialize libunwind with local context");
        return 0;
    }
    size_t n = 0;
    // get the first frame
    if (n < max_frames) {
        unw_word_t ip = 0;
        if (unw_get_reg(&cursor, UNW_REG_IP, &ip) >= 0) {
            ip = STRIP_PAC(ip);
            ptrs[n++] = (void *)ip;
        } else {
            return 0;
        }
    }
    // walk the callers
    unw_word_t prev_ip = n > 0 ? (uintptr_t)ptrs[0] : 0;
    unw_word_t prev_sp = 0;
    (void)unw_get_reg(&cursor, UNW_REG_SP, &prev_sp);
    while (n < max_frames && unw_step(&cursor) > 0) {
        unw_word_t ip = 0, sp = 0;
        // stop the walk if we fail to read IP
        if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0) {
            break;
        }
        // SP is optional for progress
        (void)unw_get_reg(&cursor, UNW_REG_SP, &sp);

        // stop the walk if there is _no_ progress
        if (ip == prev_ip && sp == prev_sp) {
            break;
        }
        ip = STRIP_PAC(ip);
        prev_ip = ip;
        prev_sp = sp;
        ptrs[n++] = (void *)ip;
    }

    return n;
}
