// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/page_size.h"

#include <unistd.h>

namespace base {

size_t GetPageSize() {
  return getpagesize();
}

}  // namespace base
