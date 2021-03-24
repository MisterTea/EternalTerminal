// Compat header for missing `basename`

#pragma once

#include <string.h>

static const char *
_unwinder_basename(const char *s)
{
    const char *c = strrchr(s, '/');
    return c ? c + 1 : s;
}
