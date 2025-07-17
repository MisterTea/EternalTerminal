// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_CONTAINERS_DYNAMIC_EXTENT_H_
#define MINI_CHROMIUM_BASE_CONTAINERS_DYNAMIC_EXTENT_H_

#include <cstddef>
#include <limits>

namespace base {

// [views.constants]
inline constexpr size_t dynamic_extent = std::numeric_limits<size_t>::max();

}  // namespace base

#endif  // MINI_CHROMIUM_BASE_CONTAINERS_DYNAMIC_EXTENT_H_
