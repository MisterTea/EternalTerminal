#include "sentry_path.h"
#include "sentry_testsupport.h"

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

SENTRY_TEST(module_addr)
{
#if !defined(SENTRY_PLATFORM_LINUX)
    SKIP_TEST();
#else
    sentry_module_t module = { 0 };
    module.num_mappings = 3;
    // |    |    |    |    |    |    |    |
    //           00000          1111111111
    module.mappings[0].offset = 0;
    module.mappings[0].size = 5;
    module.mappings[0].addr = 10;
    // here is a gap in the address space of size 10
    module.mappings[1].offset = 5;
    module.mappings[1].size = 10;
    module.mappings[1].addr = 25;

    void *ptr;

    ptr = sentry__module_get_addr(&module, 0, 5);
    TEST_CHECK(ptr == (void *)10);

    ptr = sentry__module_get_addr(&module, 0, 6);
    TEST_CHECK(ptr == NULL); // not contiguous

    ptr = sentry__module_get_addr(&module, 7, 8);
    TEST_CHECK(ptr == (void *)27);

    ptr = sentry__module_get_addr(&module, 7, 9);
    TEST_CHECK(ptr == NULL); // too big
#endif
}

SENTRY_TEST(procmaps_parser)
{
#if !defined(SENTRY_PLATFORM_LINUX) || __SIZEOF_POINTER__ != 8
    SKIP_TEST();
#else
    sentry_parsed_module_t mod;
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
    TEST_CHECK(mod.start == 0x7fdb549ce000);
    TEST_CHECK(mod.end == 0x7fdb54bb5000);
    TEST_CHECK(strncmp(mod.file.ptr, "/lib/x86_64-linux-gnu/libc-2.27.so",
                   mod.file.len)
        == 0);

    read = sentry__procmaps_parse_module_line(lines, &mod);
    lines += read;
    TEST_CHECK(read);
    TEST_CHECK(mod.start == 0x7f14753de000);
    TEST_CHECK(mod.end == 0x7f14755de000);

    read = sentry__procmaps_parse_module_line(lines, &mod);
    lines += read;
    TEST_CHECK(read);
    TEST_CHECK(mod.start == 0x7fe714493000);
    TEST_CHECK(mod.end == 0x7fe714494000);
    TEST_CHECK(mod.file.ptr == NULL);

    read = sentry__procmaps_parse_module_line(lines, &mod);
    lines += read;
    TEST_CHECK(read);
    TEST_CHECK(mod.start == 0x7fff8ca67000);
    TEST_CHECK(mod.end == 0x7fff8ca88000);
    TEST_CHECK(strncmp(mod.file.ptr, "[vdso]", mod.file.len) == 0);

    read = sentry__procmaps_parse_module_line(lines, &mod);
    TEST_CHECK(!read);
#endif
}

#ifdef SENTRY_PLATFORM_LINUX
static void
parse_elf_and_check_code_and_build_id(const char *rel_elf_path,
    const char *expected_code_id, const char *expected_debug_id)
{
    sentry_path_t *path = sentry__path_from_str(__FILE__);
    sentry_path_t *dir = sentry__path_dir(path);
    sentry__path_free(path);

    sentry_module_t module = { 0 };
    module.num_mappings = 1;
    size_t *file_size = &module.mappings[0].size;
    char **buf = (char **)&module.mappings[0].addr;

    sentry_value_t value = sentry_value_new_object();
    sentry_path_t *elf_path = sentry__path_join_str(dir, rel_elf_path);
    *buf = sentry__path_read_to_buffer(elf_path, file_size);
    sentry__path_free(elf_path);

    TEST_CHECK(sentry__procmaps_read_ids_from_elf(value, &module));
    sentry_free(*buf);
    sentry__path_free(dir);

    if (expected_code_id) {
        TEST_CHECK_STRING_EQUAL(
            sentry_value_as_string(sentry_value_get_by_key(value, "code_id")),
            expected_code_id);
    } else {
        TEST_CHECK(
            sentry_value_is_null(sentry_value_get_by_key(value, "code_id")));
    }

    // The debug-id should always be non-NULL
    TEST_CHECK_STRING_EQUAL(
        sentry_value_as_string(sentry_value_get_by_key(value, "debug_id")),
        expected_debug_id);

    sentry_value_decref(value);
}
#endif

SENTRY_TEST(build_id_parser)
{
    // skipping this on android because it does not have access to the fixtures
#if !defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
    SKIP_TEST();
#else
    parse_elf_and_check_code_and_build_id("../fixtures/with-buildid.so",
        "1c304742f114215453a8a777f6cdb3a2b8505e11",
        "4247301c-14f1-5421-53a8-a777f6cdb3a2");

    parse_elf_and_check_code_and_build_id("../fixtures/without-buildid-phdr.so",
        "1c304742f114215453a8a777f6cdb3a2b8505e11",
        "4247301c-14f1-5421-53a8-a777f6cdb3a2");

    parse_elf_and_check_code_and_build_id("../fixtures/sentry_example",
        "b4c24a6cc995c17fb18a65184a65863cfc01c673",
        "6c4ac2b4-95c9-7fc1-b18a-65184a65863c");

    parse_elf_and_check_code_and_build_id("../fixtures/without-buildid.so",
        NULL, "29271919-a2ef-129d-9aac-be85a0948d9c");

    parse_elf_and_check_code_and_build_id("../fixtures/libstdc++.so", NULL,
        "7fa824da-38f1-b87c-04df-718fda64990c");
#endif
}
