#include "sentry_boot.h"

#include "sentry_random.h"
#include "sentry_utils.h"

#ifdef SENTRY_PLATFORM_DARWIN
#    include <stdlib.h>

static int
getrandom_arc4random(void *dst, size_t bytes)
{
    arc4random_buf(dst, bytes);
    return 0;
}
#    define HAVE_ARC4RANDOM

#endif
#ifdef SENTRY_PLATFORM_UNIX
#    include <errno.h>
#    include <fcntl.h>
#    include <unistd.h>

static int
getrandom_devurandom(void *dst, size_t bytes)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return 1;
    }

    size_t to_read = bytes;
    char *d = dst;
    while (to_read > 0) {
        ssize_t n = read(fd, d, to_read);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        } else if (n <= 0) {
            break;
        }
        d += n;
        to_read -= n;
    }

    close(fd);
    return to_read > 0;
}

#    define HAVE_URANDOM
#endif
#ifdef SENTRY_PLATFORM_WINDOWS
typedef BOOLEAN(WINAPI *sRtlGenRandom)(PVOID Buffer, ULONG BufferLength);

static sRtlGenRandom pRtlGenRandom = NULL;
static bool sRtlGenRandomLoaded = false;

static int
getrandom_rtlgenrandom(void *dst, size_t bytes)
{
    if (sRtlGenRandomLoaded == false) {
        HANDLE advapi32_module = LoadLibraryA("advapi32.dll");
        if (advapi32_module != NULL) {
            pRtlGenRandom = (sRtlGenRandom)GetProcAddress(
                advapi32_module, "SystemFunction036");
        }
        sRtlGenRandomLoaded = true;
    }
    if (pRtlGenRandom == NULL) {
        return 1;
    }
    if (pRtlGenRandom(dst, (ULONG)bytes) == FALSE) {
        return 1;
    }
    return 0;
}

#    define HAVE_RTLGENRANDOM
#endif

int
sentry__getrandom(void *dst, size_t len)
{
#ifdef HAVE_ARC4RANDOM
    if (getrandom_arc4random(dst, len) == 0) {
        return 0;
    }
#endif
#ifdef HAVE_URANDOM
    if (getrandom_devurandom(dst, len) == 0) {
        return 0;
    }
#endif
#ifdef HAVE_RTLGENRANDOM
    if (getrandom_rtlgenrandom(dst, len) == 0) {
        return 0;
    }
#endif
    return 1;
}
