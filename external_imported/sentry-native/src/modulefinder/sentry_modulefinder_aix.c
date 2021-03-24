#include "sentry_boot.h"

#include "sentry_core.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_value.h"

#include <limits.h>
#include <stdio.h>

#define __XCOFF64__
#include <sys/ldr.h>
#include <xcoff.h>

/* library filename + ( + member file name + ) + NUL */
#define AIX_PRINTED_LIB_LEN ((PATH_MAX * 2) + 3)

static bool g_initialized = false;
static sentry_mutex_t g_mutex = SENTRY__MUTEX_INIT;
static sentry_value_t g_modules = { 0 };

static void
load_modules(void)
{
    char buf[10000];
    int r = loadquery(L_GETINFO, buf, 10000);
    if (r == -1) {
        return;
    }
    /* The loader info structures are also a linked list. */
    struct ld_info *cur = (struct ld_info *)buf;
    do {
        sentry_value_t module = sentry_value_new_object();
        sentry_value_set_by_key(
            module, "type", sentry_value_new_string("xcoff"));

        char *tb = (char *)cur->ldinfo_textorg; // text includes XCOFF image
        sentry_value_set_by_key(
            module, "image_addr", sentry__value_new_addr((uint64_t)tb));
        // actually a 64-bit value on 64-bit AIX
        uint64_t ts = (uint64_t)cur->ldinfo_textsize;
        sentry_value_set_by_key(
            module, "image_size", sentry_value_new_int32((uint32_t)ts));

        /*
         * Under AIX, there are no UUIDs for executables, but we can try to
         * use some other fields as an ersatz substitute.
         */
        FILHDR *xcoff_header = (FILHDR *)tb;
        char timestamp[128];
        snprintf(timestamp, 128, "%x", xcoff_header->f_timdat);
        sentry_value_set_by_key(
            module, "debug_id", sentry_value_new_string(timestamp));

        /* library filename + ( + member + ) + NUL */
        char libname[AIX_PRINTED_LIB_LEN];
        char *file_part = cur->ldinfo_filename;
        char *member_part = file_part + strlen(file_part) + 1;
        /*
         * This can't be a const char*, because it exists from
         * a stack allocated buffer. Also append the member.
         *
         * XXX: See if we can't frob usla's memory ranges for
         * const strings; but is quite difficult.
         */
        if (member_part[0] == '\0') {
            /* Not an archive, just copy the file name. */
            snprintf(libname, AIX_PRINTED_LIB_LEN, "%s", file_part);
        } else {
            /* It's an archive with member. */
            snprintf(
                libname, AIX_PRINTED_LIB_LEN, "%s(%s)", file_part, member_part);
        }
        // XXX: This is not an absolute path because AIX doesn't provide
        // it. It will have the member name for library archives.
        sentry_value_set_by_key(
            module, "code_file", sentry_value_new_string(libname));

        sentry_value_append(g_modules, module);

        cur = (struct ld_info *)((char *)cur + cur->ldinfo_next);
    } while (cur->ldinfo_next != 0);
}

sentry_value_t
sentry_get_modules_list(void)
{
    sentry__mutex_lock(&g_mutex);
    if (!g_initialized) {
        g_modules = sentry_value_new_list();
        g_initialized = true;
        load_modules();
        sentry_value_freeze(g_modules);
    }
    sentry_value_t modules = g_modules;
    sentry_value_incref(modules);
    sentry__mutex_unlock(&g_mutex);
    return modules;
}

void
sentry_clear_modulecache(void)
{
    sentry__mutex_lock(&g_mutex);
    sentry_value_decref(g_modules);
    g_modules = sentry_value_new_null();
    g_initialized = false;
    sentry__mutex_unlock(&g_mutex);
}
