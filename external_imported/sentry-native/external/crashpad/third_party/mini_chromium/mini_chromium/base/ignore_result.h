// Copyright 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_IGNORE_RESULT_H_
#define MINI_CHROMIUM_BASE_IGNORE_RESULT_H_

#include <string.h>
#include <sys/types.h>

template <typename T>
inline void ignore_result(const T&) {}

#endif  // MINI_CHROMIUM_BASE_IGNORE_RESULT_H_
