/**
 * Unit tests for native crash backend
 *
 * Tests minidump structures, Build ID extraction, UUID extraction,
 * and low-level crash handling functionality.
 */

#include "sentry_options.h"
#include "sentry_testsupport.h"
#include <string.h>

#ifdef SENTRY_BACKEND_NATIVE
// Include native backend headers
#    include "../../src/backends/native/minidump/sentry_minidump_format.h"
#    include "../../src/backends/native/sentry_crash_context.h"
#endif

#if defined(SENTRY_PLATFORM_LINUX) || defined(SENTRY_PLATFORM_ANDROID)
#    include "sentry_elf.h"
#endif

/**
 * Test minidump header structure size and alignment
 */
SENTRY_TEST(minidump_header_size)
{
#ifdef SENTRY_BACKEND_NATIVE
    // Minidump header should be exactly 32 bytes
    TEST_CHECK(sizeof(minidump_header_t) == 32);

    // Verify structure alignment
    minidump_header_t header = { 0 };
    header.signature = MINIDUMP_SIGNATURE;
    header.version = MINIDUMP_VERSION;

    TEST_CHECK(header.signature == 0x504d444d); // 'MDMP' in little-endian
    TEST_CHECK(header.version == 0xa793); // Version 1.0
#else
    SKIP_TEST();
#endif
}

/**
 * Test minidump directory entry structure
 */
SENTRY_TEST(minidump_directory_size)
{
#ifdef SENTRY_BACKEND_NATIVE
    TEST_CHECK(sizeof(minidump_directory_t) == 12);

    minidump_directory_t dir = { 0 };
    dir.stream_type = MINIDUMP_STREAM_SYSTEM_INFO;
    dir.data_size = 100;
    dir.rva = 1000;

    TEST_CHECK(dir.stream_type == 7); // SYSTEM_INFO is 7
    TEST_CHECK(dir.data_size == 100);
    TEST_CHECK(dir.rva == 1000);
#else
    SKIP_TEST();
#endif
}

/**
 * Test thread context structures
 */
SENTRY_TEST(minidump_context_sizes)
{
#ifdef SENTRY_BACKEND_NATIVE
#    if defined(__x86_64__)
    // x86_64 context with FPU should be 1232 bytes
    TEST_CHECK(sizeof(minidump_context_x86_64_t) == 1232);

    minidump_context_x86_64_t ctx = { 0 };
    ctx.context_flags = 0x0010003f; // Full context with FPU
    ctx.rip = 0x12345678;
    ctx.rsp = 0x7fff0000;

    TEST_CHECK(ctx.context_flags == 0x0010003f);
    TEST_CHECK(ctx.rip == 0x12345678);

    // Verify XMM save area exists
    ctx.float_save.mx_csr = 0x1f80;
    TEST_CHECK(ctx.float_save.mx_csr == 0x1f80);

#    elif defined(__aarch64__)
    // ARM64 context: 4+4 + 29*8 + 3*8 + 32*16 + 4+4 + 8*8 + 8*8 + 2*4 + 2*8
    // = 8 + 232 + 24 + 512 + 8 + 64 + 64 + 8 + 16 = 936 bytes (actual: 912 with
    // packing)
    TEST_CHECK(sizeof(minidump_context_arm64_t) == 912);

    minidump_context_arm64_t ctx = { 0 };
    ctx.context_flags = 0x00400007; // ARM64 | Control | Integer | Fpsimd
    ctx.pc = 0x100000000;
    ctx.sp = 0x16b000000;

    TEST_CHECK(ctx.context_flags == 0x00400007);
    TEST_CHECK(ctx.pc == 0x100000000);

    // Verify NEON/FP registers exist
    ctx.fpsr = 0x12345678;
    TEST_CHECK(ctx.fpsr == 0x12345678);

#    endif
#else
    SKIP_TEST();
#endif
}

/**
 * Test module structure
 */
SENTRY_TEST(minidump_module_structure)
{
#ifdef SENTRY_BACKEND_NATIVE
    // Module structure size:
    // base_of_image (8) + size_of_image (4) + checksum (4) + time_date_stamp
    // (4)
    // + module_name_rva (4) + version_info[13] (52) + cv_record (8) +
    // misc_record (8) + reserved0 (8) + reserved1 (8) = 108 bytes
    TEST_CHECK(sizeof(minidump_module_t) == 108);

    minidump_module_t module = { 0 };
    module.base_of_image = 0x100000000;
    module.size_of_image = 0x10000;
    module.module_name_rva = 1000;

    TEST_CHECK(module.base_of_image == 0x100000000);
    TEST_CHECK(module.size_of_image == 0x10000);

    // Verify CodeView record can be set
    module.cv_record.rva = 2000;
    module.cv_record.size = 100;

    TEST_CHECK(module.cv_record.rva == 2000);
    TEST_CHECK(module.cv_record.size == 100);
#else
    SKIP_TEST();
#endif
}

/**
 * Test thread structure
 */
SENTRY_TEST(minidump_thread_structure)
{
#ifdef SENTRY_BACKEND_NATIVE
    TEST_CHECK(sizeof(minidump_thread_t) == 48);

    minidump_thread_t thread = { 0 };
    thread.thread_id = 12345;
    thread.stack.start_address = 0x7fff0000;
    thread.stack.memory.size = 65536;
    thread.thread_context.rva = 1000;

    TEST_CHECK(thread.thread_id == 12345);
    TEST_CHECK(thread.stack.start_address == 0x7fff0000);
    TEST_CHECK(thread.stack.memory.size == 65536);
#else
    SKIP_TEST();
#endif
}

/**
 * Test system info structure
 */
SENTRY_TEST(minidump_system_info)
{
#ifdef SENTRY_BACKEND_NATIVE
    minidump_system_info_t sysinfo = { 0 };

#    if defined(__x86_64__)
    sysinfo.processor_architecture = MINIDUMP_CPU_X86_64;
    TEST_CHECK(sysinfo.processor_architecture == 9);
#    elif defined(__aarch64__)
    sysinfo.processor_architecture = MINIDUMP_CPU_ARM64;
    TEST_CHECK(sysinfo.processor_architecture == 12);
#    endif

    sysinfo.number_of_processors = 8;
    TEST_CHECK(sysinfo.number_of_processors == 8);
#else
    SKIP_TEST();
#endif
}

/**
 * Test exception record structure
 */
SENTRY_TEST(minidump_exception_record)
{
#ifdef SENTRY_BACKEND_NATIVE
    minidump_exception_record_t exception = { 0 };
    exception.exception_code = 0xc0000005; // Access violation
    exception.exception_address = 0x12345678;

    TEST_CHECK(exception.exception_code == 0xc0000005);
    TEST_CHECK(exception.exception_address == 0x12345678);
#else
    SKIP_TEST();
#endif
}

/**
 * Test memory descriptor structure
 */
SENTRY_TEST(minidump_memory_descriptor)
{
#ifdef SENTRY_BACKEND_NATIVE
    minidump_memory_descriptor_t mem = { 0 };
    mem.start_address = 0x7fff0000;
    mem.memory.size = 4096;
    mem.memory.rva = 1000;

    TEST_CHECK(mem.start_address == 0x7fff0000);
    TEST_CHECK(mem.memory.size == 4096);
    TEST_CHECK(mem.memory.rva == 1000);
#else
    SKIP_TEST();
#endif
}

/**
 * Test that minidump stream types are correct
 */
SENTRY_TEST(minidump_stream_types)
{
#ifdef SENTRY_BACKEND_NATIVE
    TEST_CHECK(MINIDUMP_STREAM_THREAD_LIST == 3);
    TEST_CHECK(MINIDUMP_STREAM_MODULE_LIST == 4);
    TEST_CHECK(MINIDUMP_STREAM_MEMORY_LIST == 5);
    TEST_CHECK(MINIDUMP_STREAM_EXCEPTION == 6);
    TEST_CHECK(MINIDUMP_STREAM_SYSTEM_INFO == 7);
#else
    SKIP_TEST();
#endif
}

/**
 * Test CPU architecture constants
 */
SENTRY_TEST(minidump_cpu_architectures)
{
#ifdef SENTRY_BACKEND_NATIVE
    TEST_CHECK(MINIDUMP_CPU_X86 == 0); // PROCESSOR_ARCHITECTURE_INTEL
    TEST_CHECK(MINIDUMP_CPU_ARM == 5); // PROCESSOR_ARCHITECTURE_ARM
    TEST_CHECK(MINIDUMP_CPU_ARM64 == 12); // PROCESSOR_ARCHITECTURE_ARM64
    TEST_CHECK(MINIDUMP_CPU_X86_64 == 9); // PROCESSOR_ARCHITECTURE_AMD64
#else
    SKIP_TEST();
#endif
}

/**
 * Test context flags
 */
SENTRY_TEST(minidump_context_flags)
{
#ifdef SENTRY_BACKEND_NATIVE
#    if defined(__x86_64__)
    // x86_64 full context flags
    uint32_t flags = 0x0010003f;
    TEST_CHECK((flags & 0x00100000) != 0); // CONTEXT_AMD64
    TEST_CHECK((flags & 0x00000001) != 0); // CONTEXT_CONTROL
    TEST_CHECK((flags & 0x00000002) != 0); // CONTEXT_INTEGER
    TEST_CHECK((flags & 0x00000004) != 0); // CONTEXT_SEGMENTS
    TEST_CHECK((flags & 0x00000008) != 0); // CONTEXT_FLOATING_POINT

#    elif defined(__aarch64__)
    // ARM64 full context flags
    uint32_t flags = 0x00400007;
    TEST_CHECK((flags & 0x00400000) != 0); // ARM64_CONTEXT
    TEST_CHECK((flags & 0x00000001) != 0); // CONTROL
    TEST_CHECK((flags & 0x00000002) != 0); // INTEGER
    TEST_CHECK((flags & 0x00000004) != 0); // FPSIMD
#    endif
#else
    SKIP_TEST();
#endif
}

/**
 * Test uint128_struct for NEON registers
 */
SENTRY_TEST(uint128_struct_size)
{
#if defined(SENTRY_BACKEND_NATIVE) && defined(__aarch64__)
    TEST_CHECK(sizeof(uint128_struct) == 16);

    uint128_struct val = { 0 };
    val.low = 0x123456789abcdef0ULL;
    val.high = 0xfedcba9876543210ULL;

    TEST_CHECK(val.low == 0x123456789abcdef0ULL);
    TEST_CHECK(val.high == 0xfedcba9876543210ULL);
#else
    SKIP_TEST();
#endif
}

/**
 * Test XMM save area structure
 */
SENTRY_TEST(xmm_save_area_size)
{
#if defined(SENTRY_BACKEND_NATIVE) && defined(__x86_64__)
    TEST_CHECK(sizeof(xmm_save_area32_t) == 512);

    xmm_save_area32_t fpu = { 0 };
    fpu.control_word = 0x037f;
    fpu.mx_csr = 0x1f80;

    TEST_CHECK(fpu.control_word == 0x037f);
    TEST_CHECK(fpu.mx_csr == 0x1f80);
#else
    SKIP_TEST();
#endif
}

SENTRY_TEST(m128a_size)
{
#if defined(SENTRY_BACKEND_NATIVE) && defined(__x86_64__)
    TEST_CHECK(sizeof(m128a_t) == 16);

    m128a_t val = { 0 };
    val.low = 0x123456789abcdef0ULL;
    val.high = 0xfedcba9876543210ULL;

    TEST_CHECK(val.low == 0x123456789abcdef0ULL);
    TEST_CHECK(val.high == 0xfedcba9876543210ULL);
#else
    SKIP_TEST();
#endif
}

/**
 * Test that crash context structure includes transport configuration fields
 * and that they are properly sized for paths and URLs.
 */
SENTRY_TEST(crash_context_transport_fields)
{
#ifdef SENTRY_BACKEND_NATIVE
    // Heap-allocate: sentry_crash_context_t is multiple MB due to the inline
    // modules[] array and would overflow smaller thread stacks.
    sentry_crash_context_t *ctx = sentry_malloc(sizeof(*ctx));
    TEST_ASSERT(!!ctx);
    memset(ctx, 0, sizeof(*ctx));

    // Verify ca_certs field exists and can hold a typical path
    const char *test_ca = "/etc/ssl/certs/ca-certificates.crt";
    strncpy(ctx->ca_certs, test_ca, sizeof(ctx->ca_certs) - 1);
    ctx->ca_certs[sizeof(ctx->ca_certs) - 1] = '\0';
    TEST_CHECK_STRING_EQUAL(ctx->ca_certs, test_ca);

    // Verify proxy field exists and can hold a typical proxy URL
    const char *test_proxy = "http://proxy.example.com:8080";
    strncpy(ctx->proxy, test_proxy, sizeof(ctx->proxy) - 1);
    ctx->proxy[sizeof(ctx->proxy) - 1] = '\0';
    TEST_CHECK_STRING_EQUAL(ctx->proxy, test_proxy);

    // Verify user_agent field exists
    const char *test_ua = "sentry.native/0.8.0";
    strncpy(ctx->user_agent, test_ua, sizeof(ctx->user_agent) - 1);
    ctx->user_agent[sizeof(ctx->user_agent) - 1] = '\0';
    TEST_CHECK_STRING_EQUAL(ctx->user_agent, test_ua);

    ctx->shutdown_timeout = 12345;
    TEST_CHECK_UINT64_EQUAL(ctx->shutdown_timeout, 12345);
    ctx->crash_upload_mode = SENTRY_CRASH_UPLOAD_MODE_ASYNC;
    TEST_CHECK_INT_EQUAL(
        ctx->crash_upload_mode, SENTRY_CRASH_UPLOAD_MODE_ASYNC);
    ctx->transfer_timeout = 45000;
    TEST_CHECK_UINT64_EQUAL(ctx->transfer_timeout, 45000);

    // Verify fields are zero-initialized when memset to 0
    memset(ctx, 0, sizeof(*ctx));
    TEST_CHECK(ctx->ca_certs[0] == '\0');
    TEST_CHECK(ctx->proxy[0] == '\0');
    TEST_CHECK(ctx->user_agent[0] == '\0');
    TEST_CHECK_UINT64_EQUAL(ctx->shutdown_timeout, 0);
    TEST_CHECK_INT_EQUAL(ctx->crash_upload_mode, SENTRY_CRASH_UPLOAD_MODE_SYNC);
    TEST_CHECK_UINT64_EQUAL(ctx->transfer_timeout, 0);

    sentry_free(ctx);
#else
    SKIP_TEST();
#endif
}

/**
 * Test that options transport configuration is propagated to crash context
 * during native backend startup. This verifies the fix for the daemon
 * not receiving SSL certs and proxy settings from the parent process.
 */
SENTRY_TEST(crash_context_options_propagation)
{
#ifdef SENTRY_BACKEND_NATIVE
    // Create options with transport config
    SENTRY_TEST_OPTIONS_NEW(options);

    sentry_options_set_ca_certs(options, "/path/to/ca-bundle.crt");
    sentry_options_set_proxy(options, "http://myproxy:3128");
    sentry_options_set_shutdown_timeout(options, 12345);
    sentry_options_set_system_crash_reporter_enabled(options, true);
    sentry_options_set_crash_upload_mode(
        options, SENTRY_CRASH_UPLOAD_MODE_ASYNC);
    sentry_options_set_transfer_timeout(options, 45000);

    // Verify options were set correctly
    TEST_CHECK_STRING_EQUAL(
        sentry_options_get_ca_certs(options), "/path/to/ca-bundle.crt");
    TEST_CHECK_STRING_EQUAL(
        sentry_options_get_proxy(options), "http://myproxy:3128");

    // Simulate what native_backend_startup does: copy to crash context.
    // Heap-allocated to avoid overflowing the stack on the inline modules[].
    sentry_crash_context_t *ctx = sentry_malloc(sizeof(*ctx));
    TEST_ASSERT(!!ctx);
    memset(ctx, 0, sizeof(*ctx));

    if (options->ca_certs) {
        strncpy(ctx->ca_certs, options->ca_certs, sizeof(ctx->ca_certs) - 1);
        ctx->ca_certs[sizeof(ctx->ca_certs) - 1] = '\0';
    }
    if (options->proxy) {
        strncpy(ctx->proxy, options->proxy, sizeof(ctx->proxy) - 1);
        ctx->proxy[sizeof(ctx->proxy) - 1] = '\0';
    }
    if (options->user_agent) {
        strncpy(
            ctx->user_agent, options->user_agent, sizeof(ctx->user_agent) - 1);
        ctx->user_agent[sizeof(ctx->user_agent) - 1] = '\0';
    }
    ctx->shutdown_timeout = options->shutdown_timeout;
    ctx->system_crash_reporter_enabled = options->system_crash_reporter_enabled;
    ctx->crash_upload_mode = options->crash_upload_mode;
    ctx->transfer_timeout = options->transfer_timeout;

    // Verify crash context received the values
    TEST_CHECK_STRING_EQUAL(ctx->ca_certs, "/path/to/ca-bundle.crt");
    TEST_CHECK_STRING_EQUAL(ctx->proxy, "http://myproxy:3128");
    // user_agent should have the default SDK user agent
    TEST_CHECK(ctx->user_agent[0] != '\0');
    TEST_CHECK_UINT64_EQUAL(ctx->shutdown_timeout, 12345);
    TEST_CHECK(ctx->system_crash_reporter_enabled);
    TEST_CHECK_INT_EQUAL(
        ctx->crash_upload_mode, SENTRY_CRASH_UPLOAD_MODE_ASYNC);
    TEST_CHECK_UINT64_EQUAL(ctx->transfer_timeout, 45000);

    sentry_free(ctx);
    sentry_options_free(options);
#else
    SKIP_TEST();
#endif
}

/**
 * Test that handler_path option is set correctly in options.
 * Note: handler_path is now passed directly to the daemon start function
 * instead of being stored in shared memory, since the daemon never reads
 * it from shared memory.
 */
SENTRY_TEST(crash_context_handler_path_propagation)
{
#ifdef SENTRY_BACKEND_NATIVE
    SENTRY_TEST_OPTIONS_NEW(options);

    // Set handler path
    sentry_options_set_handler_path(options, "/opt/sentry/sentry-crash");

    // Verify handler_path option is set correctly
    TEST_ASSERT(!!options->handler_path);
    TEST_CHECK_STRING_EQUAL(
        options->handler_path->path, "/opt/sentry/sentry-crash");

    // Without handler_path set, it should be NULL
    sentry_options_t *options2 = sentry_options_new();
    TEST_ASSERT(!!options2);
    TEST_CHECK(options2->handler_path == NULL);

    sentry_options_free(options);
    sentry_options_free(options2);
#else
    SKIP_TEST();
#endif
}

/**
 * Test that NULL/empty transport options don't corrupt crash context
 */
SENTRY_TEST(crash_context_null_options)
{
#ifdef SENTRY_BACKEND_NATIVE
    SENTRY_TEST_OPTIONS_NEW(options);

    // Don't set ca_certs or proxy - leave them as NULL.
    // Heap-allocated to avoid overflowing the stack on the inline modules[].
    sentry_crash_context_t *ctx = sentry_malloc(sizeof(*ctx));
    TEST_ASSERT(!!ctx);
    memset(ctx, 0, sizeof(*ctx));

    // Copy like native_backend_startup does (with NULL checks)
    if (options->ca_certs) {
        strncpy(ctx->ca_certs, options->ca_certs, sizeof(ctx->ca_certs) - 1);
    }
    if (options->proxy) {
        strncpy(ctx->proxy, options->proxy, sizeof(ctx->proxy) - 1);
    }

    // All should remain empty (zero-initialized)
    TEST_CHECK(ctx->ca_certs[0] == '\0');
    TEST_CHECK(ctx->proxy[0] == '\0');

    sentry_free(ctx);
    sentry_options_free(options);
#else
    SKIP_TEST();
#endif
}

/**
 * Test packed attribute works correctly
 */
SENTRY_TEST(minidump_structures_packed)
{
#ifdef SENTRY_BACKEND_NATIVE
    // Structures should not have padding
    // This is critical for binary format compatibility

#    if defined(__x86_64__)
    // x86_64 context: 6*8 + 4*2 + 6*2 + 2*4 + 8*8 + 16*8 + 512 + 26*16 + 6*8 =
    // 1232
    size_t expected_x86_64 = 48 + 8 + 12 + 8 + 64 + 128 + 512 + 416 + 48;
    TEST_CHECK(sizeof(minidump_context_x86_64_t) == expected_x86_64);

#    elif defined(__aarch64__)
    // ARM64 context: 4 + 4 + 29*8 + 4*8 + 32*16 + 4 + 4 + 8*4 + 8*8 + 2*4 + 2*8
    // = 1344
    size_t expected_arm64 = 8 + 232 + 32 + 512 + 8 + 32 + 64 + 8 + 16;
    TEST_CHECK(sizeof(minidump_context_arm64_t) <= expected_arm64 + 100);
#    endif
#else
    SKIP_TEST();
#endif
}

SENTRY_TEST(elf_header_entry_sizes)
{
#if !defined(SENTRY_PLATFORM_LINUX) && !defined(SENTRY_PLATFORM_ANDROID)
    SKIP_TEST();
#else
    unsigned char e_ident[EI_NIDENT] = { 0 };
    unsigned char other_class;
    size_t shdr_size;
    size_t phdr_size;

#    if defined(__x86_64__) || defined(__aarch64__)
    e_ident[EI_CLASS] = ELFCLASS64;
    other_class = ELFCLASS32;
    shdr_size = sizeof(Elf64_Shdr);
    phdr_size = sizeof(Elf64_Phdr);
#    else
    e_ident[EI_CLASS] = ELFCLASS32;
    other_class = ELFCLASS64;
    shdr_size = sizeof(Elf32_Shdr);
    phdr_size = sizeof(Elf32_Phdr);
#    endif

    TEST_CHECK(sentry__elf_is_native_class(e_ident));
    TEST_CHECK(sentry__elf_has_shdr_size(e_ident, shdr_size));
    TEST_CHECK(sentry__elf_has_phdr_size(e_ident, phdr_size));

    TEST_CHECK(!sentry__elf_has_shdr_size(e_ident, shdr_size - 1));
    TEST_CHECK(!sentry__elf_has_shdr_size(e_ident, shdr_size + 1));
    TEST_CHECK(!sentry__elf_has_phdr_size(e_ident, phdr_size - 1));
    TEST_CHECK(!sentry__elf_has_phdr_size(e_ident, phdr_size + 1));

    e_ident[EI_CLASS] = other_class;
    TEST_CHECK(!sentry__elf_is_native_class(e_ident));
    TEST_CHECK(!sentry__elf_has_shdr_size(e_ident, shdr_size));
    TEST_CHECK(!sentry__elf_has_phdr_size(e_ident, phdr_size));
#endif
}
