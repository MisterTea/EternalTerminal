#ifndef SENTRY_BOOT_H_INCLUDED
#define SENTRY_BOOT_H_INCLUDED

// This header should always be included first.  Since we use clang-format
// it needs to be separated by a blank line to prevent automatic reordering.
//
// The purpose of this header is to configure some common system libraries
// before they are being used.  For instance this sets up some defines so
// that Windows.h is less polluting or that we get GNU extensions on linux.
//
// It also includes sentry.h since this is commonly used.

// we use some non portable extensions
#if !defined(_GNU_SOURCE) && defined(__linux__)
#    define _GNU_SOURCE
#endif

#ifndef __STDC_FORMAT_MACROS
#    define __STDC_FORMAT_MACROS
#endif

// make sure on windows we pull in a minimal Windows.h
#if defined(_WIN32)
#    if !defined(WIN32_LEAN_AND_MEAN)
#        define WIN32_LEAN_AND_MEAN
#    endif
#    if !defined(NOMINMAX)
#        define NOMINMAX
#    endif
#    if !defined(_CRT_SECURE_NO_WARNINGS)
#        define _CRT_SECURE_NO_WARNINGS
#    endif
#    include <windows.h>
#endif

#include <sentry.h>
#include <stdbool.h>

#endif
