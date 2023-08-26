// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/process/memory.h"

#include <stdlib.h>

namespace base {

bool UncheckedMalloc(size_t size, void** result) {
  *result = malloc(size);
  return *result != NULL;
}

}  // namespace base
