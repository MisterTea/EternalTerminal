#include "sentry_process.h"

#include "sentry_alloc.h"
#include "sentry_logger.h"
#include "sentry_string.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char **
argv_new(size_t len)
{
    char **argv = sentry_malloc((len + 1) * sizeof(char *));
    if (argv) {
        argv[len] = NULL;
    }
    return argv;
}

static bool
argv_set(char **argv, size_t index, const char *value)
{
    if (!argv || !value) {
        return false;
    }

    argv[index] = sentry_malloc(strlen(value) + 1);
    if (!argv[index]) {
        return false;
    }
    strcpy(argv[index], value);
    return true;
}

static char *
argv_to_string(char **argv)
{
    if (!argv) {
        return NULL;
    }

    size_t len = 0;
    for (int i = 0; argv[i]; i++) {
        len += sentry__guarded_strlen(argv[i]) + 1;
    }

    char *str = sentry_malloc(len);
    if (!str) {
        return NULL;
    }
    str[0] = '\0';

    for (int i = 0; argv[i]; i++) {
        if (i > 0) {
            strcat(str, " ");
        }
        strcat(str, argv[i]);
    }
    return str;
}

static void
argv_free(char **argv)
{
    if (!argv) {
        return;
    }

    for (int i = 0; argv[i]; i++) {
        sentry_free(argv[i]);
    }
    sentry_free(argv);
}

/**
 * Spawns a new fully detached subprocess by double-forking.
 */
static void
spawn_process(char **argv)
{
    pid_t pid1 = fork();
    if (pid1 == -1) {
        SENTRY_ERRORF("first fork() failed: %s", strerror(errno));
        return;
    }

    if (pid1 == 0) {
        // first child process: create new session and process group to detach
        // from parent
        if (setsid() == -1) {
            SENTRY_ERRORF("setsid() failed: %s", strerror(errno));
            _exit(1);
        }

        // second fork to ensure the process is not a session leader and cannot
        // acquire a controlling terminal
        pid_t pid2 = fork();
        if (pid2 == -1) {
            SENTRY_ERRORF("second fork() failed: %s", strerror(errno));
            _exit(1);
        }

        if (pid2 == 0) {
            // second child process: redirect stdin/out/err to /dev/null
            int dev_null = open("/dev/null", O_RDWR);
            if (dev_null != -1) {
                dup2(dev_null, STDIN_FILENO);
                dup2(dev_null, STDOUT_FILENO);
                dup2(dev_null, STDERR_FILENO);
                if (dev_null > STDERR_FILENO) {
                    close(dev_null);
                }
            }

            execv(argv[0], argv);

            SENTRY_ERRORF("execv failed: %s", strerror(errno));
            _exit(1);
        } else {
            // the first child exits immediately
            _exit(0);
        }
    } else {
        // parent process: wait for the first child to exit
        int status;
        if (waitpid(pid1, &status, 0) == -1) {
            SENTRY_ERRORF("waitpid() failed: %s", strerror(errno));
        } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            SENTRY_ERRORF("child process failed with status %d", status);
        }
    }
}

void
sentry__process_spawn(const sentry_path_t *executable, const char *arg0, ...)
{
    if (!executable || sentry__string_empty(executable->path)) {
        return;
    }

    int argc = 1;
#ifdef SENTRY_PLATFORM_MACOS
    bool is_bundle = sentry__path_ends_with(executable, ".app");
    if (is_bundle) {
        argc += 2; // /usr/bin/open -a <bundle>
    }
#endif

    if (arg0) {
#ifdef SENTRY_PLATFORM_MACOS
        if (is_bundle) {
            argc++; // --args
        }
#endif
        argc++;
        va_list args;
        va_start(args, arg0);
        while (va_arg(args, const char *) != NULL) {
            argc++;
        }
        va_end(args);
    }

    int i = 0;
    char **argv = argv_new(argc);

#ifdef SENTRY_PLATFORM_MACOS
    if (is_bundle
        && (!argv_set(argv, i++, "/usr/bin/open")
            || !argv_set(argv, i++, "-a"))) {
        argv_free(argv);
        return;
    }
#endif

    if (!argv_set(argv, i++, executable->path)) {
        argv_free(argv);
        return;
    }

    if (arg0) {
#ifdef SENTRY_PLATFORM_MACOS
        if (is_bundle && !argv_set(argv, i++, "--args")) {
            argv_free(argv);
            return;
        }
#endif
        if (!argv_set(argv, i++, arg0)) {
            argv_free(argv);
            return;
        }
        va_list args;
        va_start(args, arg0);
        const char *argn;
        while ((argn = va_arg(args, const char *)) != NULL) {
            if (!argv_set(argv, i++, argn)) {
                va_end(args);
                argv_free(argv);
                return;
            }
        }
        va_end(args);
    }

    char *cli = argv_to_string(argv);
    SENTRY_DEBUGF("spawning %s", cli);
    sentry_free(cli);

    spawn_process(argv);
    argv_free(argv);
}
