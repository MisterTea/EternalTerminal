#include "sentry_os.h"
#include "sentry_path.h"
#include "sentry_testsupport.h"

#if defined(SENTRY_PLATFORM_LINUX) && !defined(SENTRY_PLATFORM_ANDROID)
#    include <dirent.h>

struct distro {
    char *name;
    char *version;
};

const struct distro test_dists[] = {
    { .name = "almalinux", .version = "8.7" },
    { .name = "amzn", .version = "2" },
    { .name = "alpine", .version = "3.9.6" },
    { .name = "fedora", .version = "28" },
    { .name = "sles_sap", .version = "12.2" },
    { .name = "opensuse-leap", .version = "15.4" },
    { .name = "clear-linux-os", .version = "38250" },
    { .name = "almalinux", .version = "9.1" },
    { .name = "alpine", .version = "3.8.5" },
    { .name = "xenenterprise", .version = "8.2.0" },
    { .name = "sles", .version = "12.3" },
    { .name = "manjaro-arm", .version = "" },
    { .name = "fedora", .version = "29" },
    { .name = "slackware", .version = "14.2" },
    { .name = "debian", .version = "8" },
    { .name = "cumulus-linux", .version = "3.7.2" },
    { .name = "ubuntu", .version = "18.04" },
    { .name = "debian", .version = "9" },
    { .name = "ubuntu", .version = "16.04" },
    { .name = "debian", .version = "7" },
    { .name = "sles", .version = "12.3" },
    { .name = "debian", .version = "10" },
    { .name = "elementary", .version = "5.0" },
    { .name = "raspbian", .version = "8" },
    { .name = "ubuntu", .version = "20.04" },
    { .name = "ol", .version = "8.7" },
    { .name = "alpine", .version = "3.17.2" },
    { .name = "amzn", .version = "2018.03" },
    { .name = "scientific", .version = "7.5" },
    { .name = "alpine", .version = "3.10.9" },
    { .name = "debian", .version = "11" },
    { .name = "ol", .version = "7.9" },
    { .name = "alpine", .version = "3.11.13" },
    { .name = "ol", .version = "9.1" },
    { .name = "alpine", .version = "3.16.4" },
    { .name = "fedora", .version = "34" },
    { .name = "fedora", .version = "33" },
    { .name = "sled", .version = "15" },
    { .name = "XCP-ng", .version = "7.4.0" },
    { .name = "linuxmint", .version = "18.2" },
    { .name = "fedora", .version = "32" },
    { .name = "fedora", .version = "35" },
    { .name = "sles", .version = "11.4" },
    { .name = "clearos", .version = "7" },
    { .name = "xenenterprise", .version = "7.5.0" },
    { .name = "sles_sap", .version = "12.1.0.1" },
    { .name = "centos", .version = "8" },
    { .name = "kali", .version = "2018.4" },
    { .name = "centos", .version = "7" },
    { .name = "sles_sap", .version = "12.0.1" },
    { .name = "arch", .version = "TEMPLATE_VERSION_ID" },
    { .name = "ios_xr", .version = "6.0.0.14I" },
    { .name = "rocky", .version = "8.7" },
    { .name = "nexus", .version = "7.0(BUILDER)" },
    { .name = "ubuntu", .version = "14.04" },
    { .name = "xenenterprise", .version = "" },
    { .name = "rancheros", .version = "v1.4.2" },
    { .name = "raspbian", .version = "1.0" },
    { .name = "rocky", .version = "9.1" },
    { .name = "antergos", .version = "" },
    { .name = "linuxmint", .version = "19" },
    { .name = "alpine", .version = "3.13.12" },
    { .name = "nixos", .version = "18.09.1436.a7fd4310c0c" },
    { .name = "alpine", .version = "3.14.9" },
    { .name = "arcolinux", .version = "" },
    { .name = "elementary", .version = "6" },
    { .name = "ubuntu", .version = "22.04" },
    { .name = "manjaro", .version = "" },
    { .name = "amzn", .version = "2022" },
    { .name = "alpine", .version = "3.15.7" },
    { .name = "alpine", .version = "3.12.12" },
    { .name = "gentoo", .version = "" },
    { .name = "archarm", .version = "" },
    { .name = "raspbian", .version = "10" },
    { .name = "sles", .version = "15" },
    { .name = "rhel", .version = "8.0" },
    { .name = "fedora", .version = "30" },
    { .name = "fedora", .version = "37" },
    { .name = "mageia", .version = "6" },
    { .name = "rhel", .version = "9.1" },
    { .name = "sles", .version = "15.1" },
    { .name = "rhel", .version = "7.5" },
    { .name = "fedora", .version = "38" },
    { .name = "centos", .version = "8" },
    { .name = "fedora", .version = "36" },
    { .name = "endeavouros", .version = "" },
    { .name = "virtuozzo", .version = "7" },
    { .name = "opensuse", .version = "42.3" },
    { .name = "fedora", .version = "31" },
    { .name = "pop", .version = "22.04" },
    { .name = "sled", .version = "12.3" },
};

const size_t num_test_dists = sizeof(test_dists) / sizeof(struct distro);

int
value_strcmp_by_key(sentry_value_t value, const char *key, const char *str)
{
    return strcmp(
        sentry_value_as_string(sentry_value_get_by_key(value, key)), str);
}

int
assert_equals_snap(sentry_value_t os_dist)
{
    for (size_t i = 0; i < num_test_dists; i++) {
        const struct distro *expected = &test_dists[i];
        if (value_strcmp_by_key(os_dist, "name", expected->name) == 0
            && value_strcmp_by_key(os_dist, "version", expected->version)
                == 0) {
            return 1;
        }
    }
    return 0;
}

extern sentry_value_t get_linux_os_release(const char *os_rel_path);
#endif // defined(SENTRY_PLATFORM_LINUX) && !defined(SENTRY_PLATFORM_ANDROID)

SENTRY_TEST(os_releases_snapshot)
{
#if !defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    SKIP_TEST();
#else
    const char *rel_test_data_path = "../fixtures/os_releases";
    sentry_path_t *path = sentry__path_from_str(__FILE__);
    sentry_path_t *dir = sentry__path_dir(path);
    sentry__path_free(path);

    sentry_path_t *test_data_path
        = sentry__path_join_str(dir, rel_test_data_path);
    sentry__path_free(dir);
    TEST_ASSERT(sentry__path_is_dir(test_data_path));

    DIR *test_data_dir = opendir(test_data_path->path);
    TEST_ASSERT(test_data_dir != NULL);

    struct dirent *entry;

    int successful_snap_asserts = 0;
    while ((entry = readdir(test_data_dir)) != NULL) {
        if (entry->d_type != DT_REG || strcmp("LICENSE", entry->d_name) == 0
            || strcmp("README.md", entry->d_name) == 0
            || strcmp("distribution_names.txt", entry->d_name) == 0) {
            continue;
        }

        sentry_path_t *test_file_path
            = sentry__path_join_str(test_data_path, entry->d_name);
        sentry_value_t os_dist = get_linux_os_release(test_file_path->path);
        sentry__path_free(test_file_path);
        TEST_ASSERT(!sentry_value_is_null(os_dist));
        int snap_result = assert_equals_snap(os_dist);
        if (!snap_result) {
            TEST_CHECK(snap_result);
            TEST_MSG("%s: %s not found",
                sentry_value_as_string(
                    sentry_value_get_by_key(os_dist, "name")),
                sentry_value_as_string(
                    sentry_value_get_by_key(os_dist, "version")));
        } else {
            successful_snap_asserts++;
        }
        sentry_value_decref(os_dist);
    }

    TEST_CHECK_INT_EQUAL(successful_snap_asserts, num_test_dists);

    closedir(test_data_dir);
    sentry__path_free(test_data_path);
#endif // !defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
}

SENTRY_TEST(os_release_non_existent_files)
{
#if !defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    SKIP_TEST();
#else
    sentry_value_t os_release = get_linux_os_release("invalid_path");
    TEST_ASSERT(sentry_value_is_null(os_release));
#endif // !defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
}

#ifdef SENTRY_PLATFORM_WINDOWS
extern BOOL(WINAPI *g_kernel32_SetThreadStackGuarantee)(PULONG);
extern void(WINAPI *g_kernel32_GetCurrentThreadStackLimits)(
    PULONG_PTR, PULONG_PTR);
static size_t g_kernel32_SetThreadStackGuaranteeCalled = 0;

static BOOL WINAPI
no_previous_guarantee(PULONG guarantee)
{
    if (*guarantee != 0) {
        TEST_CHECK_INT_EQUAL(*guarantee, SENTRY_HANDLER_STACK_SIZE * 1024);
    }

    g_kernel32_SetThreadStackGuaranteeCalled++;

    *guarantee = 0;
    return 1;
}

static BOOL WINAPI
guarantee_already_set(PULONG guarantee)
{
    if (*guarantee != 0) {
        TEST_CHECK_INT_EQUAL(*guarantee, SENTRY_HANDLER_STACK_SIZE * 1024);
    }

    g_kernel32_SetThreadStackGuaranteeCalled++;

    *guarantee = 32;
    return 1;
}

static BOOL WINAPI
guarantee_fails_in_query(PULONG guarantee)
{
    if (*guarantee != 0) {
        TEST_CHECK_INT_EQUAL(*guarantee, SENTRY_HANDLER_STACK_SIZE * 1024);
    }

    g_kernel32_SetThreadStackGuaranteeCalled++;

    if (*guarantee == 0) {
        *guarantee = 0;
        return 0;
    }

    return 1;
}

static BOOL WINAPI
guarantee_fails_in_setter(PULONG guarantee)
{
    if (*guarantee != 0) {
        TEST_CHECK_INT_EQUAL(*guarantee, SENTRY_HANDLER_STACK_SIZE * 1024);
    }

    g_kernel32_SetThreadStackGuaranteeCalled++;

    if (*guarantee == 0) {
        *guarantee = 0;
        return 1;
    } else {
        *guarantee = 0;
        return 0;
    }
}

static void WINAPI
stack_reserve_half_the_factor(PULONG_PTR low, PULONG_PTR high)
{
    *high = SENTRY_HANDLER_STACK_SIZE
        * (SENTRY_THREAD_STACK_GUARANTEE_FACTOR / 2) * 1024;
    *low = 0;
}

static void WINAPI
stack_reserve_exact_factor(PULONG_PTR low, PULONG_PTR high)
{
    *high = SENTRY_HANDLER_STACK_SIZE * SENTRY_THREAD_STACK_GUARANTEE_FACTOR
        * 1024;
    *low = 0;
}

static void WINAPI
stack_reserve_twice_the_factor(PULONG_PTR low, PULONG_PTR high)
{
    *high = SENTRY_HANDLER_STACK_SIZE * SENTRY_THREAD_STACK_GUARANTEE_FACTOR * 2
        * 1024;
    *low = 0;
}

static void WINAPI
stack_reserve_exact_factor_minus_one(PULONG_PTR low, PULONG_PTR high)
{
    *high = ((SENTRY_HANDLER_STACK_SIZE * SENTRY_THREAD_STACK_GUARANTEE_FACTOR)
                - 1)
        * 1024;
    *low = 0;
}
#endif

SENTRY_TEST(stack_guarantee)
{
#if !defined(SENTRY_PLATFORM_WINDOWS)
    SKIP_TEST();
#else
    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = no_previous_guarantee;
    uint32_t stack_guarantee_in_bytes = SENTRY_HANDLER_STACK_SIZE * 1024;
    TEST_CHECK_INT_EQUAL(
        sentry_set_thread_stack_guarantee(stack_guarantee_in_bytes), 1);
    TEST_CHECK_INT_EQUAL(g_kernel32_SetThreadStackGuaranteeCalled, 2);

    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = guarantee_already_set;
    TEST_CHECK_INT_EQUAL(
        sentry_set_thread_stack_guarantee(stack_guarantee_in_bytes), 0);
    TEST_CHECK_INT_EQUAL(g_kernel32_SetThreadStackGuaranteeCalled, 1);

    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = guarantee_fails_in_query;
    TEST_CHECK_INT_EQUAL(
        sentry_set_thread_stack_guarantee(stack_guarantee_in_bytes), 0);
    TEST_CHECK_INT_EQUAL(g_kernel32_SetThreadStackGuaranteeCalled, 1);

    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = guarantee_fails_in_setter;
    TEST_CHECK_INT_EQUAL(
        sentry_set_thread_stack_guarantee(stack_guarantee_in_bytes), 0);
    TEST_CHECK_INT_EQUAL(g_kernel32_SetThreadStackGuaranteeCalled, 2);

    // reset the globals
    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = NULL;
#endif
}

SENTRY_TEST(stack_guarantee_auto_init)
{
#if !defined(SENTRY_PLATFORM_WINDOWS)
    SKIP_TEST();
#else
    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = no_previous_guarantee;
    g_kernel32_GetCurrentThreadStackLimits = stack_reserve_half_the_factor;

    sentry__set_default_thread_stack_guarantee();
    TEST_CHECK_INT_EQUAL(g_kernel32_SetThreadStackGuaranteeCalled, 0);

    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = no_previous_guarantee;
    g_kernel32_GetCurrentThreadStackLimits
        = stack_reserve_exact_factor_minus_one;

    sentry__set_default_thread_stack_guarantee();
    TEST_CHECK_INT_EQUAL(g_kernel32_SetThreadStackGuaranteeCalled, 0);

    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = no_previous_guarantee;
    g_kernel32_GetCurrentThreadStackLimits = stack_reserve_exact_factor;

    sentry__set_default_thread_stack_guarantee();
    TEST_CHECK_INT_EQUAL(g_kernel32_SetThreadStackGuaranteeCalled, 2);

    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = no_previous_guarantee;
    g_kernel32_GetCurrentThreadStackLimits = stack_reserve_twice_the_factor;

    sentry__set_default_thread_stack_guarantee();
    TEST_CHECK_INT_EQUAL(g_kernel32_SetThreadStackGuaranteeCalled, 2);

    // reset globals
    g_kernel32_SetThreadStackGuaranteeCalled = 0;
    g_kernel32_SetThreadStackGuarantee = NULL;
    g_kernel32_GetCurrentThreadStackLimits = NULL;
#endif
}
