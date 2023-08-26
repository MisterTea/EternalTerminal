// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_PROCESS_MEMORY_H_
#define MINI_CHROMIUM_BASE_PROCESS_MEMORY_H_

#include <stddef.h>

namespace base {

// Special allocator function for callers that want to check for OOM.
// On success, *result will contain a pointer that should be dallocated with
// free().
[[nodiscard]] bool UncheckedMalloc(size_t size, void** result);

}  // namespace base

#endif  // MINI_CHROMIUM_BASE_PROCESS_MEMORY_H_
