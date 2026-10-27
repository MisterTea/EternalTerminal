#include "sentry_path.h"
#include "sentry_process.h"
#include "sentry_testsupport.h"

// merely tests that it doesn't crash with invalid arguments
SENTRY_TEST(process_invalid)
{
    sentry__process_spawn(NULL, NULL);

    sentry_path_t *empty = sentry__path_from_str("");
    sentry__process_spawn(empty, NULL);
    sentry__path_free(empty);

    sentry_path_t *nul = sentry__path_from_str_owned(NULL);
    sentry__process_spawn(nul, NULL);
    sentry__path_free(nul);
}

#if defined(SENTRY_PLATFORM_MACOS)                                             \
    || (defined(SENTRY_PLATFORM_LINUX) && !defined(SENTRY_PLATFORM_ANDROID))
void
find_cp_path(char *buf, size_t buf_len)
{
    FILE *fp = popen("command -v cp 2>/dev/null", "r");
    if (fp && fgets(buf, buf_len, fp)) {
        buf[strcspn(buf, "\n")] = 0; // strip newline
    }
    if (fp) {
        pclose(fp);
    }
}
#endif

SENTRY_TEST(process_spawn)
{
#if defined(SENTRY_PLATFORM_WINDOWS) || defined(SENTRY_PLATFORM_MACOS)         \
    || (defined(SENTRY_PLATFORM_LINUX) && !defined(SENTRY_PLATFORM_ANDROID))
    sentry_path_t *exe = sentry__path_current_exe();
    TEST_ASSERT(!!exe);
    TEST_CHECK(sentry__path_is_file(exe));

    sentry_path_t *dst = sentry__path_from_str(
        SENTRY_TEST_PATH_PREFIX ".sentry_test_unit process_spawn");
    TEST_ASSERT(!!dst);
    TEST_CHECK(sentry__path_remove(dst) == 0);
    TEST_CHECK(!sentry__path_is_file(dst));

#    ifdef SENTRY_PLATFORM_WINDOWS
    // cmd /C copy <src> <dst>
    sentry_path_t *cmd
        = sentry__path_from_str("C:\\Windows\\System32\\cmd.exe");
    TEST_ASSERT(!!cmd);
    sentry__process_spawn(cmd, "/C", "copy", exe->path, dst->path, NULL);
    sentry__path_free(cmd);
#    else
    char cp_path[512] = "/bin/cp";
    find_cp_path(cp_path, sizeof(cp_path));
    // cp <src> <dst>
    sentry_path_t *cp = sentry__path_from_str(cp_path);
    TEST_ASSERT(!!cp);
    sentry__process_spawn(cp, exe->path, dst->path, NULL);
    sentry__path_free(cp);
#    endif

    size_t size = sentry__path_get_size(exe);

    // wait up to 5s for the detached copy process
    int i = 0;
    while (i++ < 100
        && (!sentry__path_is_file(dst) || sentry__path_get_size(dst) < size)) {
        sleep_ms(50);
    }
    TEST_CHECK(sentry__path_is_file(dst));
    TEST_CHECK_INT_EQUAL(sentry__path_get_size(dst), size);

    sentry__path_remove(dst);
    sentry__path_free(exe);
    sentry__path_free(dst);
#else
    SKIP_TEST();
#endif
}
