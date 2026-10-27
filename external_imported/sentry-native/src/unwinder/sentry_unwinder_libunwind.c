#include "sentry_boot.h"
#include "sentry_logger.h"
#include "sentry_unwinder.h"
#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/**
 * Parse a lower-case hex number starting at `s`.
 * Advances `*pos` past the parsed digits.  Returns the parsed value.
 */
static uintptr_t
parse_hex(const char *s, size_t len, size_t *pos)
{
    uintptr_t val = 0;
    while (*pos < len) {
        char c = s[*pos];
        if (c >= '0' && c <= '9') {
            val = (val << 4) | (uintptr_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            val = (val << 4) | (uintptr_t)(c - 'a' + 10);
        } else {
            break;
        }
        (*pos)++;
    }
    return val;
}

/**
 * Try to parse "lo-hi" from the start of `line` (length `len`).
 * Returns `true` and fills `range` if `ptr` falls inside [lo, hi).
 */
static bool
match_range(const char *line, size_t len, uintptr_t ptr, mem_range_t *range)
{
    size_t p = 0;
    uintptr_t lo = parse_hex(line, len, &p);
    if (p >= len || line[p] != '-') {
        return false;
    }
    p++;
    uintptr_t hi = parse_hex(line, len, &p);
    if (lo < hi && ptr >= lo && ptr < hi) {
        range->lo = lo;
        range->hi = hi;
        return true;
    }
    return false;
}

/**
 * Searches for the memory range containing `ptr` by reading lines in the
 * /proc/self/maps format from `fd`.
 *
 * This implementation is async-signal-safe: it only uses POSIX file-IO and
 * stdio-free hex parsing.
 */
#ifndef SENTRY_UNITTEST
static
#endif
    bool
    find_mem_range_from_fd(int fd, uintptr_t ptr, mem_range_t *range)
{
    char buf[4096];
    size_t carry = 0;
    bool skip_to_nl = false; // true while discarding an oversized line's tail

    for (;;) {
        ssize_t n;
        do {
            n = read(fd, buf + carry, sizeof(buf) - carry);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            return false;
        }

        size_t total = carry + (size_t)n;
        size_t pos = 0;

        // If we are in the middle of skipping an oversized line, scan
        // forward to the newline before resuming normal parsing.
        if (skip_to_nl) {
            while (pos < total && buf[pos] != '\n') {
                pos++;
            }
            if (pos >= total) {
                carry = 0;
                continue;
            }
            pos++; // skip past newline
            skip_to_nl = false;
        }

        while (pos < total) {
            size_t nl = pos;
            while (nl < total && buf[nl] != '\n') {
                nl++;
            }
            if (nl >= total) {
                break;
            }
            if (match_range(buf + pos, nl - pos, ptr, range)) {
                return true;
            }
            pos = nl + 1;
        }

        carry = total - pos;
        if (carry >= sizeof(buf)) {
            // A single line fills the entire buffer.  Parse the "lo-hi"
            // prefix that is already in buf, then skip the rest of the
            // line on subsequent reads.
            if (match_range(buf, sizeof(buf), ptr, range)) {
                return true;
            }
            carry = 0;
            skip_to_nl = true;
        } else if (carry > 0 && pos > 0) {
            for (size_t i = 0; i < carry; i++) {
                buf[i] = buf[pos + i];
            }
        }
    }
}

/**
 * Looks up the memory range for `ptr` in /proc/self/maps.
 */
static bool
find_mem_range(uintptr_t ptr, mem_range_t *range)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    bool found = find_mem_range_from_fd(fd, ptr, range);
    close(fd);
    return found;
}

size_t
sentry__unwind_stack_libunwind(
    void *addr, const sentry_ucontext_t *uctx, void **ptrs, size_t max_frames)
{
    if (addr) {
        return 0;
    }

    unw_cursor_t cursor;
    unw_context_t uc;
    if (uctx) {
        int ret = unw_init_local2(&cursor, (unw_context_t *)uctx->user_context,
            UNW_INIT_SIGNAL_FRAME);
        if (ret != 0) {
            SENTRY_WARN("Failed to initialize libunwind with ucontext");
            return 0;
        }
    } else {
#ifdef __clang__
// This pragma is required to build with Werror on ARM64 Ubuntu
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored                                           \
        "-Wgnu-statement-expression-from-macro-expansion"
#endif
        int ret = unw_getcontext(&uc);
#ifdef __clang__
#    pragma clang diagnostic pop
#endif
        if (ret != 0) {
            SENTRY_WARN("Failed to retrieve context with libunwind");
            return 0;
        }

        ret = unw_init_local(&cursor, &uc);
        if (ret != 0) {
            SENTRY_WARN("Failed to initialize libunwind with local context");
            return 0;
        }
    }

    size_t n = 0;
    // get the first frame and stack pointer
    unw_word_t ip = 0;
    if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0) {
        return n;
    }
    if (n < max_frames) {
        ptrs[n++] = (void *)ip;
    }
    unw_word_t sp = 0;
    (void)unw_get_reg(&cursor, UNW_REG_SP, &sp);

    // ensure we have a valid stack pointer otherwise we only send the top frame
    mem_range_t stack = { 0, 0 };
    if (uctx && !find_mem_range((uintptr_t)sp, &stack)) {
        SENTRY_WARNF("unwinder: SP (%p) is in unmapped memory likely due to "
                     "stack overflow",
            (void *)sp);
        return n;
    }

    // walk the callers
    unw_word_t prev_ip = ip;
    unw_word_t prev_sp = sp;
    while (n < max_frames && unw_step(&cursor) > 0) {
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

        prev_ip = ip;
        prev_sp = sp;
        ptrs[n++] = (void *)ip;
    }
    return n;
}
