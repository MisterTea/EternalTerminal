// Copyright 2006-2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_STRINGS_STRING_UTIL_POSIX_H_
#define MINI_CHROMIUM_BASE_STRINGS_STRING_UTIL_POSIX_H_

#include <stdio.h>
#include <string.h>

namespace base {

inline int vsnprintf(char* buffer,
                     size_t size,
                     const char* format, va_list arguments) {
  return ::vsnprintf(buffer, size, format, arguments);
}

// Chromium code style is to not use malloc'd strings; this is only for use
// for interaction with APIs that require it.
inline char* strdup(const char* str) {
  return ::strdup(str);
}

}  // namespace base

#endif  // MINI_CHROMIUM_BASE_STRINGS_STRING_UTIL_POSIX_H_
