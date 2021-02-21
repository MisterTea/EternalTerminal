#include "sentry_path.h"
#include "sentry_testsupport.h"
#include <sentry.h>

#ifdef SENTRY_PLATFORM_LINUX
#    include "modulefinder/sentry_modulefinder_linux.h"
#endif

SENTRY_TEST(module_finder)
{
    // make sure that we are able to do multiple cleanup cycles
    sentry_value_decref(sentry_get_modules_list());
    sentry_clear_modulecache();

    sentry_value_t modules = sentry_get_modules_list();
    TEST_CHECK(sentry_value_get_length(modules) > 0);
    TEST_CHECK(sentry_value_is_frozen(modules));

    bool found_test = false;
    for (size_t i = 0; i < sentry_value_get_length(modules); i++) {
        sentry_value_t mod = sentry_value_get_by_index(modules, i);
        sentry_value_t name = sentry_value_get_by_key(mod, "code_file");
        const char *name_str = sentry_value_as_string(name);
        if (strstr(name_str, "sentry_test_unit")) {
            // our tests should also have at least a debug_id on all platforms
            sentry_value_t debug_id = sentry_value_get_by_key(mod, "debug_id");
            TEST_CHECK(
                sentry_value_get_type(debug_id) == SENTRY_VALUE_TYPE_STRING);

            found_test = true;
        }
    }
    sentry_value_decref(modules);

    TEST_CHECK(found_test);

    sentry_clear_modulecache();
}

SENTRY_TEST(procmaps_parser)
{
#if !defined(SENTRY_PLATFORM_LINUX) || __SIZEOF_POINTER__ != 8
    SKIP_TEST();
#else
    sentry_module_t mod;
    char contents[]
        = "7fdb549ce000-7fdb54bb5000 r-xp 00000000 08:01 3803938       "
          "             /lib/x86_64-linux-gnu/libc-2.27.so\n"
          "7f14753de000-7f14755de000 ---p 001e7000 08:01 3803938       "
          "             /lib/x86_64-linux-gnu/libc-2.27.so\n"
          "7fe714493000-7fe714494000 rw-p 00000000 00:00 0\n"
          "7fff8ca67000-7fff8ca88000 rw-p 00000000 00:00 0             "
          "             [vdso]";
    char *lines = contents;
    int read;

    read = sentry__procmaps_parse_module_line(lines, &mod);
    lines += read;
    TEST_CHECK(read);
    TEST_CHECK(mod.start == (void *)0x7fdb549ce000);
    TEST_CHECK(mod.end == (void *)0x7fdb54bb5000);
    TEST_CHECK(strncmp(mod.file.ptr, "/lib/x86_64-linux-gnu/libc-2.27.so",
                   mod.file.len)
        == 0);

    read = sentry__procmaps_parse_module_line(lines, &mod);
    lines += read;
    TEST_CHECK(read);
    TEST_CHECK(mod.start == (void *)0x7f14753de000);
    TEST_CHECK(mod.end == (void *)0x7f14755de000);

    read = sentry__procmaps_parse_module_line(lines, &mod);
    lines += read;
    TEST_CHECK(read);
    TEST_CHECK(mod.start == (void *)0x7fe714493000);
    TEST_CHECK(mod.end == (void *)0x7fe714494000);
    TEST_CHECK(mod.file.ptr == NULL);

    read = sentry__procmaps_parse_module_line(lines, &mod);
    lines += read;
    TEST_CHECK(read);
    TEST_CHECK(mod.start == (void *)0x7fff8ca67000);
    TEST_CHECK(mod.end == (void *)0x7fff8ca88000);
    TEST_CHECK(strncmp(mod.file.ptr, "[vdso]", mod.file.len) == 0);

    read = sentry__procmaps_parse_module_line(lines, &mod);
    TEST_CHECK(!read);
#endif
}

SENTRY_TEST(buildid_fallback)
{
    // skipping this on android because it does not have access to the fixtures
#if !defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    SKIP_TEST();
#else
    sentry_path_t *path = sentry__path_from_str(__FILE__);
    sentry_path_t *dir = sentry__path_dir(path);
    sentry__path_free(path);

    sentry_value_t with_id_val = sentry_value_new_object();
    sentry_mmap_t with_id_map;
    sentry_path_t *with_id_path
        = sentry__path_join_str(dir, "../fixtures/with-buildid.so");
    TEST_CHECK(sentry__mmap_file(&with_id_map, with_id_path->path));
    sentry__path_free(with_id_path);

    TEST_CHECK(
        sentry__procmaps_read_ids_from_elf(with_id_val, with_id_map.ptr));
    sentry__mmap_close(&with_id_map);

    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(with_id_val, "code_id")),
        "1c304742f114215453a8a777f6cdb3a2b8505e11");
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                with_id_val, "debug_id")),
        "4247301c-14f1-5421-53a8-a777f6cdb3a2");
    sentry_value_decref(with_id_val);

    sentry_value_t x86_exe_val = sentry_value_new_object();
    sentry_mmap_t x86_exe_map;
    sentry_path_t *x86_exe_path
        = sentry__path_join_str(dir, "../fixtures/sentry_example");
    TEST_CHECK(sentry__mmap_file(&x86_exe_map, x86_exe_path->path));
    sentry__path_free(x86_exe_path);

    TEST_CHECK(
        sentry__procmaps_read_ids_from_elf(x86_exe_val, x86_exe_map.ptr));
    sentry__mmap_close(&x86_exe_map);

    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(x86_exe_val, "code_id")),
        "b4c24a6cc995c17fb18a65184a65863cfc01c673");
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                x86_exe_val, "debug_id")),
        "6c4ac2b4-95c9-7fc1-b18a-65184a65863c");
    sentry_value_decref(x86_exe_val);

    sentry_value_t without_id_val = sentry_value_new_object();
    sentry_mmap_t without_id_map;
    sentry_path_t *without_id_path
        = sentry__path_join_str(dir, "../fixtures/without-buildid.so");
    TEST_CHECK(sentry__mmap_file(&without_id_map, without_id_path->path));
    sentry__path_free(without_id_path);

    TEST_CHECK(
        sentry__procmaps_read_ids_from_elf(without_id_val, without_id_map.ptr));
    sentry__mmap_close(&without_id_map);

    TEST_CHECK(sentry_value_is_null(
        sentry_value_get_by_key(without_id_val, "code_id")));
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                without_id_val, "debug_id")),
        "29271919-a2ef-129d-9aac-be85a0948d9c");
    sentry_value_decref(without_id_val);

    sentry_value_t x86_lib_val = sentry_value_new_object();
    sentry_mmap_t x86_lib_map;
    sentry_path_t *x86_lib_path
        = sentry__path_join_str(dir, "../fixtures/libstdc++.so");
    TEST_CHECK(sentry__mmap_file(&x86_lib_map, x86_lib_path->path));
    sentry__path_free(x86_lib_path);

    TEST_CHECK(
        sentry__procmaps_read_ids_from_elf(x86_lib_val, x86_lib_map.ptr));
    sentry__mmap_close(&x86_lib_map);

    TEST_CHECK(
        sentry_value_is_null(sentry_value_get_by_key(x86_lib_val, "code_id")));
    TEST_CHECK_STRING_EQUAL(sentry_value_as_string(sentry_value_get_by_key(
                                x86_lib_val, "debug_id")),
        "7fa824da-38f1-b87c-04df-718fda64990c");
    sentry_value_decref(x86_lib_val);

    sentry__path_free(dir);
#endif
}
