#include "sentry_process.h"

#include "sentry_alloc.h"
#include "sentry_logger.h"
#include "sentry_string.h"

#include <stdarg.h>
#include <windows.h>

static size_t
quote_arg(const char *src, char *dst)
{
    if (!strchr(src, ' ') && !strchr(src, '"')) {
        if (dst) {
            strcpy(dst, src);
        }
        return strlen(src);
    }

    size_t len = 0;

    // opening quote
    if (dst) {
        dst[len] = '"';
    }
    len++;

    // escape quotes and backslashes
    for (const char *p = src; *p; ++p) {
        if (*p == '"') {
            if (dst) {
                dst[len] = '\\';
                dst[len + 1] = '"';
            }
            len += 2;
        } else if (*p == '\\') {
            const char *q = p;
            size_t slashes = 0;
            while (*q == '\\') {
                slashes++;
                q++;
            }
            if (*q == '"') {
                // backslashes followed by a quote -> double the backslashes to
                // escape each backslash
                slashes *= 2;
            }
            for (size_t i = 0; i < slashes; ++i) {
                if (dst) {
                    dst[len] = '\\';
                }
                len++;
            }
            p = q - 1;
        } else {
            if (dst) {
                dst[len] = *p;
            }
            len++;
        }
    }

    // closing quote
    if (dst) {
        dst[len] = '"';
        dst[len + 1] = '\0';
    }
    return ++len;
}

void
sentry__process_spawn(const sentry_path_t *executable, const char *arg0, ...)
{
    if (!executable || sentry__string_empty(executable->path)) {
        return;
    }

    size_t cli_len = quote_arg(executable->path, NULL) + 1; // \0
    if (arg0) {
        cli_len += quote_arg(arg0, NULL) + 1; // space
        va_list args;
        va_start(args, arg0);
        const char *argn;
        while ((argn = va_arg(args, const char *)) != NULL) {
            cli_len += quote_arg(argn, NULL) + 1; // space
        }
        va_end(args);
    }

    char *cli = sentry_malloc(cli_len * sizeof(char));
    if (!cli) {
        return;
    }

    size_t offset = quote_arg(executable->path, cli);
    if (arg0) {
        cli[offset++] = ' ';
        offset += quote_arg(arg0, cli + offset);
        va_list args;
        va_start(args, arg0);
        const char *argn;
        while ((argn = va_arg(args, const char *)) != NULL) {
            cli[offset++] = ' ';
            offset += quote_arg(argn, cli + offset);
        }
        va_end(args);
    }
    cli[offset] = '\0';

    SENTRY_DEBUGF("spawning %s", cli);

    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);

    wchar_t *cli_w = sentry__string_to_wstr(cli);
    if (!cli_w) {
        SENTRY_ERROR(
            "sentry__process_spawn: failed to convert CLI to wide string");
        sentry_free(cli);
        return;
    }
    BOOL rv = CreateProcessW(executable->path_w, // lpApplicationName
        cli_w, // lpCommandLine
        NULL, // lpProcessAttributes
        NULL, // lpThreadAttributes
        FALSE, // bInheritHandles
        DETACHED_PROCESS, // dwCreationFlags
        NULL, // lpEnvironment
        NULL, // lpCurrentDirectory
        &si, // lpStartupInfo
        &pi // lpProcessInformation
    );

    sentry_free(cli_w);
    sentry_free(cli);

    if (!rv) {
        SENTRY_ERRORF("CreateProcess failed: %lu", GetLastError());
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}
