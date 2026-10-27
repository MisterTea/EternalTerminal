// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/strings/strcat.h"

#include <string>
#include <string_view>

#include "base/strings/strcat_internal.h"

namespace base {

std::string StrCat(span<const std::string_view> pieces) {
  return internal::StrCatT(pieces);
}

}  // namespace base
