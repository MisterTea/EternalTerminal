#include "sentry_os.h"
#include "sentry_string.h"
#include "sentry_utils.h"

#ifdef SENTRY_PLATFORM_WINDOWS

#    include <winver.h>
#    define CURRENT_VERSION "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"

void *
sentry__try_file_version(LPCWSTR filename)
{

    DWORD size = GetFileVersionInfoSizeW(filename, NULL);
    if (!size) {
        return NULL;
    }

    void *ffibuf = sentry_malloc(size);
    if (!GetFileVersionInfoW(filename, 0, size, ffibuf)) {
        sentry_free(ffibuf);
        return NULL;
    }
    return ffibuf;
}

sentry_value_t
sentry__get_os_context(void)
{
    sentry_value_t os = sentry_value_new_object();
    if (sentry_value_is_null(os)) {
        return os;
    }

    sentry_value_set_by_key(os, "name", sentry_value_new_string("Windows"));

    void *ffibuf = sentry__try_file_version(L"ntoskrnl.exe");
    if (!ffibuf) {
        ffibuf = sentry__try_file_version(L"kernel32.dll");
    }
    if (!ffibuf) {
        goto fail;
    }

    VS_FIXEDFILEINFO *ffi;
    UINT ffi_size;
    if (!VerQueryValueW(ffibuf, L"\\", &ffi, &ffi_size)) {
        goto fail;
    }
    ffi->dwFileFlags &= ffi->dwFileFlagsMask;

    uint32_t major_version = ffi->dwFileVersionMS >> 16;
    uint32_t minor_version = ffi->dwFileVersionMS & 0xffff;
    uint32_t build_version = ffi->dwFileVersionLS >> 16;
    uint32_t ubr = ffi->dwFileVersionLS & 0xffff;

    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%lu", major_version, minor_version,
        build_version, ubr);
    sentry_value_set_by_key(os, "kernel_version", sentry_value_new_string(buf));

    sentry_free(ffibuf);

    // The `CurrentMajorVersionNumber`, `CurrentMinorVersionNumber` and `UBR`
    // are DWORD, while `CurrentBuild` is a SZ (text).

    uint32_t reg_version = 0;
    DWORD buf_size = sizeof(uint32_t);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, CURRENT_VERSION,
            "CurrentMajorVersionNumber", RRF_RT_REG_DWORD, NULL, &reg_version,
            &buf_size)
        == ERROR_SUCCESS) {
        major_version = reg_version;
    }
    buf_size = sizeof(uint32_t);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, CURRENT_VERSION,
            "CurrentMinorVersionNumber", RRF_RT_REG_DWORD, NULL, &reg_version,
            &buf_size)
        == ERROR_SUCCESS) {
        minor_version = reg_version;
    }
    buf_size = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, CURRENT_VERSION, "CurrentBuild",
            RRF_RT_REG_SZ, NULL, buf, &buf_size)
        == ERROR_SUCCESS) {
        build_version = (uint32_t)sentry__strtod_c(buf, NULL);
    }
    buf_size = sizeof(uint32_t);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, CURRENT_VERSION, "UBR",
            RRF_RT_REG_DWORD, NULL, &reg_version, &buf_size)
        == ERROR_SUCCESS) {
        ubr = reg_version;
    }

    snprintf(buf, sizeof(buf), "%u.%u.%u", major_version, minor_version,
        build_version);
    sentry_value_set_by_key(os, "version", sentry_value_new_string(buf));

    snprintf(buf, sizeof(buf), "%lu", ubr);
    sentry_value_set_by_key(os, "build", sentry_value_new_string(buf));

    sentry_value_freeze(os);
    return os;

fail:
    sentry_free(ffibuf);

    sentry_value_decref(os);
    return sentry_value_new_null();
}

#elif defined(SENTRY_PLATFORM_MACOS)

#    include <sys/sysctl.h>
#    include <sys/utsname.h>

sentry_value_t
sentry__get_os_context(void)
{
    sentry_value_t os = sentry_value_new_object();
    if (sentry_value_is_null(os)) {
        return os;
    }

    sentry_value_set_by_key(os, "name", sentry_value_new_string("macOS"));

    char buf[32];
    size_t buf_len = sizeof(buf);

    if (sysctlbyname("kern.osproductversion", buf, &buf_len, NULL, 0) != 0) {
        goto fail;
    }

    size_t num_dots = 0;
    for (size_t i = 0; i < buf_len; i++) {
        if (buf[i] == '.') {
            num_dots += 1;
        }
    }
    if (num_dots < 2 && buf_len + 3 < sizeof(buf)) {
        strcat(buf, ".0");
    }

    sentry_value_set_by_key(os, "version", sentry_value_new_string(buf));

    buf_len = sizeof(buf);
    if (sysctlbyname("kern.osversion", buf, &buf_len, NULL, 0) != 0) {
        goto fail;
    }

    sentry_value_set_by_key(os, "build", sentry_value_new_string(buf));

    struct utsname uts;
    if (uname(&uts) != 0) {
        goto fail;
    }

    sentry_value_set_by_key(
        os, "kernel_version", sentry_value_new_string(uts.release));

    return os;

fail:

    sentry_value_decref(os);
    return sentry_value_new_null();
}
#elif defined(SENTRY_PLATFORM_UNIX)

#    include <sys/utsname.h>

sentry_value_t
sentry__get_os_context(void)
{
    sentry_value_t os = sentry_value_new_object();
    if (sentry_value_is_null(os)) {
        return os;
    }

    struct utsname uts;
    if (uname(&uts) != 0) {
        goto fail;
    }

    char *build = uts.release;
    size_t num_dots = 0;
    for (; build[0] != '\0'; build++) {
        char c = build[0];
        if (c == '.') {
            num_dots += 1;
        }
        if (!(c >= '0' && c <= '9') && (c != '.' || num_dots > 2)) {
            break;
        }
    }
    char *build_start = build;
    if (build[0] == '-' || build[0] == '.') {
        build_start++;
    }

    if (build_start[0] != '\0') {
        sentry_value_set_by_key(
            os, "build", sentry_value_new_string(build_start));
    }

    build[0] = '\0';

    sentry_value_set_by_key(os, "name", sentry_value_new_string(uts.sysname));
    sentry_value_set_by_key(
        os, "version", sentry_value_new_string(uts.release));

    return os;

fail:

    sentry_value_decref(os);
    return sentry_value_new_null();
}

#else

sentry_value_t
sentry__get_os_context(void)
{
    return sentry_value_new_null();
}

#endif
