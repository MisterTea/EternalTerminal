// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_NOTREACHED_H_
#define MINI_CHROMIUM_BASE_NOTREACHED_H_

#include "base/check.h"
#include "base/logging.h"

#define NOTREACHED() LOG(FATAL) << "NOTREACHED hit. "

#endif  // MINI_CHROMIUM_BASE_NOTREACHED_H_
