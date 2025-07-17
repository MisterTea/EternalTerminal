// Copyright 2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/apple/scoped_nsautorelease_pool.h"

#include "base/logging.h"

// Note that this uses the direct runtime interface to the autorelease pool.
// https://clang.llvm.org/docs/AutomaticReferenceCounting.html#runtime-support
// This is so this can work when compiled for ARC.

extern "C" {
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);
}

namespace base {
namespace apple {

ScopedNSAutoreleasePool::ScopedNSAutoreleasePool()
    : autorelease_pool_(objc_autoreleasePoolPush()) {}

ScopedNSAutoreleasePool::~ScopedNSAutoreleasePool() {
  objc_autoreleasePoolPop(autorelease_pool_);
}

// Cycle the internal pool, allowing everything there to get cleaned up and
// start anew.
void ScopedNSAutoreleasePool::Recycle() {
  objc_autoreleasePoolPop(autorelease_pool_);
  autorelease_pool_ = objc_autoreleasePoolPush();
}

}  // namespace apple
}  // namespace base
