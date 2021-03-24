#include "sentry_os.h"
#include "sentry_string.h"

#ifdef SENTRY_PLATFORM_WINDOWS

#    include <winver.h>

sentry_value_t
sentry__get_os_context(void)
{
    sentry_value_t os = sentry_value_new_object();
    if (sentry_value_is_null(os)) {
        return os;
    }

    sentry_value_set_by_key(os, "name", sentry_value_new_string("Windows"));

    void *ffibuf = NULL;

    DWORD size = GetFileVersionInfoSizeW(L"kernel32.dll", NULL);
    if (!size) {
        goto fail;
    }

    ffibuf = sentry_malloc(size);
    if (!GetFileVersionInfoW(L"kernel32.dll", 0, size, ffibuf)) {
        goto fail;
    }

    VS_FIXEDFILEINFO *ffi;
    UINT ffi_size;
    if (!VerQueryValueW(ffibuf, L"\\", &ffi, &ffi_size)) {
        goto fail;
    }
    ffi->dwFileFlags &= ffi->dwFileFlagsMask;

    char buf[100];
    snprintf(buf, sizeof(buf), "%u.%u.%u", ffi->dwFileVersionMS >> 16,
        ffi->dwFileVersionMS & 0xffff, ffi->dwFileVersionLS >> 16);

    sentry_value_set_by_key(os, "version", sentry_value_new_string(buf));

    snprintf(buf, sizeof(buf), "%lu", ffi->dwFileVersionLS & 0xffff);

    sentry_value_set_by_key(os, "build", sentry_value_new_string(buf));

    sentry_free(ffibuf);

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

    char buf[100];
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
