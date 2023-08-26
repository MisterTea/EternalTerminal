// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/page_size.h"

namespace base {

size_t GetPageSize() {
  return 4 * 1024;
}

}  // namespace base
