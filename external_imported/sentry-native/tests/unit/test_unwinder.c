#include "sentry_boot.h"
#include "sentry_symbolizer.h"
#include "sentry_testsupport.h"
#include <stdlib.h>

#ifdef SENTRY_WITH_UNWINDER_LIBUNWIND
#    include "unwinder/sentry_unwinder.h"
#    include <fcntl.h>
#    include <unistd.h>
extern bool find_mem_range_from_fd(int fd, uintptr_t ptr, mem_range_t *range);
#endif

// On arm64e, function pointers have PAC (Pointer Authentication Code) bits
// that must be stripped for comparison with addresses from dladdr().
#if defined(__arm64e__)
#    include <ptrauth.h>
#    define STRIP_PAC_FROM_FPTR(fptr)                                          \
        ptrauth_strip(fptr, ptrauth_key_function_pointer)
#else
#    define STRIP_PAC_FROM_FPTR(fptr) (fptr)
#endif

#define MAX_FRAMES 128

TEST_VISIBLE size_t
invoke_unwinder(void **backtrace)
{
    // capture a stacktrace from within this function, which means that the
    // trace will include a frame of this very function that we will test
    // against.
    size_t frame_count = sentry_unwind_stack(NULL, backtrace, MAX_FRAMES);
    // this assertion here makes sure the function is not inlined.
    TEST_CHECK(frame_count > 0);
    return frame_count;
}

static void
find_frame(const sentry_frame_info_t *info, void *data)
{
    int *found_frame = data;
    // we will specifically check for the unwinder function
    void *unwinder_address =
#if defined(SENTRY_PLATFORM_AIX)
        // AIX and ELFv1 SystemV ABI use function descriptors (a struct
        // containing the pointers required to invoke). We need to dereference
        // again to get the actual reference to code.
        // XXX: Should apply on _CALL_ELF == 1 when on PowerPC i.e. Linux
        *(void **)&invoke_unwinder;
#else
        STRIP_PAC_FROM_FPTR((void *)&invoke_unwinder);
#endif
    if (info->symbol_addr == unwinder_address) {
        *found_frame += 1;
    }
}

SENTRY_TEST(unwinder)
{
#if defined(SENTRY_PLATFORM_NX) || defined(SENTRY_PLATFORM_XBOX)               \
    || defined(SENTRY_PLATFORM_PS)
    SKIP_TEST();
#else
    if (getenv("TEST_QEMU")) {
        SKIP_TEST();
    }
#endif

    void *backtrace1[MAX_FRAMES] = { 0 };
    size_t frame_count1 = invoke_unwinder(backtrace1);
    size_t invoker_frame = 0;

    int found_frame = 0;
    for (size_t i = 0; i < frame_count1; i++) {
        // symbolize the frame, which also resolves an arbitrary instruction
        // address back to a function, and check if we found our invoker
        sentry__symbolize(backtrace1[i], find_frame, &found_frame);
        if (found_frame) {
            invoker_frame = i;
        }
    }

    TEST_CHECK_INT_EQUAL(found_frame, 1);

    // the backtrace should have:
    // X. something internal to sentry and the unwinder
    // 2. the `invoke_unwinder` function
    // 3. this test function here
    // 4. whatever parent called that test function, which should have a stable
    //    instruction pointer as long as we don’t return from here.
    size_t offset = invoker_frame + 2;
    if (frame_count1 >= offset) {
        void *backtrace2[MAX_FRAMES] = { 0 };
        size_t frame_count2
            = sentry_unwind_stack(backtrace1[offset], backtrace2, MAX_FRAMES);

        // we only have this functionality on some platforms / unwinders
        if (frame_count2 > 0) {
            TEST_CHECK_INT_EQUAL(frame_count2, frame_count1 - offset);
            for (size_t i = 0; i < frame_count2; i++) {
                TEST_CHECK_PTR_EQUAL(backtrace2[i], backtrace1[offset + i]);
            }
        }
    }
}

SENTRY_TEST(find_mem_range)
{
#if !defined(SENTRY_WITH_UNWINDER_LIBUNWIND)
    SKIP_TEST();
#else
    // A stack variable must be in a mapped region via /proc/self/maps.
    int stack_var = 42;
    (void)stack_var;
    int fd = open("/proc/self/maps", O_RDONLY);
    TEST_ASSERT(fd >= 0);
    mem_range_t range = { 0, 0 };
    bool found = find_mem_range_from_fd(fd, (uintptr_t)&stack_var, &range);
    close(fd);
    TEST_CHECK(found);
    TEST_CHECK(range.lo > 0);
    TEST_CHECK(range.lo < range.hi);
    TEST_CHECK((uintptr_t)&stack_var >= range.lo);
    TEST_CHECK((uintptr_t)&stack_var < range.hi);

    // An obviously invalid pointer should not be found.
    fd = open("/proc/self/maps", O_RDONLY);
    TEST_ASSERT(fd >= 0);
    mem_range_t bad_range = { 0, 0 };
    found = find_mem_range_from_fd(fd, (uintptr_t)0x1, &bad_range);
    close(fd);
    TEST_CHECK(!found);
    TEST_CHECK(bad_range.lo == 0);
    TEST_CHECK(bad_range.hi == 0);

    // Test that the target range ON an oversized line (> 4096 bytes) is
    // still found.  The lo-hi prefix must be parsed before discarding the
    // remainder of the line.
    char tmp_path[] = "/tmp/sentry_test_maps_XXXXXX";
    fd = mkstemp(tmp_path);
    TEST_ASSERT(fd >= 0);
    const char *long_prefix = "dead0000-deadf000 r-xp 00000000 08:01 1234 /";
    TEST_ASSERT(write(fd, long_prefix, strlen(long_prefix))
        == (ssize_t)strlen(long_prefix));
    char filler[4096];
    memset(filler, 'a', sizeof(filler));
    TEST_ASSERT(write(fd, filler, sizeof(filler)) == (ssize_t)sizeof(filler));
    TEST_ASSERT(write(fd, "\n", 1) == 1);
    lseek(fd, 0, SEEK_SET);

    mem_range_t long_range = { 0, 0 };
    found = find_mem_range_from_fd(fd, (uintptr_t)0xdead1000, &long_range);
    TEST_CHECK(found);
    TEST_CHECK_PTR_EQUAL((void *)long_range.lo, (void *)(uintptr_t)0xdead0000);
    TEST_CHECK_PTR_EQUAL((void *)long_range.hi, (void *)(uintptr_t)0xdeadf000);

    // Test that a non-matching oversized line is correctly skipped and
    // a subsequent normal line is still found.
    TEST_ASSERT(ftruncate(fd, 0) == 0);
    lseek(fd, 0, SEEK_SET);
    const char *skip_prefix = "1000-2000 r-xp 00000000 08:01 1234 /";
    TEST_ASSERT(write(fd, skip_prefix, strlen(skip_prefix))
        == (ssize_t)strlen(skip_prefix));
    TEST_ASSERT(write(fd, filler, sizeof(filler)) == (ssize_t)sizeof(filler));
    TEST_ASSERT(write(fd, "\n", 1) == 1);
    const char *target = "dead0000-deadf000 r-xp 00000000 08:01 5678 /normal\n";
    TEST_ASSERT(write(fd, target, strlen(target)) == (ssize_t)strlen(target));
    lseek(fd, 0, SEEK_SET);

    mem_range_t crafted_range = { 0, 0 };
    found = find_mem_range_from_fd(fd, (uintptr_t)0xdead1000, &crafted_range);
    close(fd);
    unlink(tmp_path);
    TEST_CHECK(found);
    TEST_CHECK_PTR_EQUAL(
        (void *)crafted_range.lo, (void *)(uintptr_t)0xdead0000);
    TEST_CHECK_PTR_EQUAL(
        (void *)crafted_range.hi, (void *)(uintptr_t)0xdeadf000);
#endif
}
