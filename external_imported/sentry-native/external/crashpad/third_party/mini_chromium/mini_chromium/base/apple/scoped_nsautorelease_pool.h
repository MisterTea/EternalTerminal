// Copyright 2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_APPLE_SCOPED_NSAUTORELEASE_POOL_H_
#define MINI_CHROMIUM_BASE_APPLE_SCOPED_NSAUTORELEASE_POOL_H_

#include "build/build_config.h"

namespace base {
namespace apple {

// On the Mac, ScopedNSAutoreleasePool creates an autorelease pool when
// instantiated and pops it when destroyed.  This allows an autorelease pool to
// be maintained in ordinary C++ code without bringing in any direct Objective-C
// dependency.
//
// On other platforms, ScopedNSAutoreleasePool is an empty object with no
// effects.  This allows it to be used directly in cross-platform code without
// ugly #ifdefs.
class ScopedNSAutoreleasePool {
 public:
#if !BUILDFLAG(IS_APPLE)
  ScopedNSAutoreleasePool() {}
  void Recycle() {}
#else  // BUILDFLAG(IS_APPLE)
  ScopedNSAutoreleasePool();

  ScopedNSAutoreleasePool(const ScopedNSAutoreleasePool&) = delete;
  ScopedNSAutoreleasePool& operator=(const ScopedNSAutoreleasePool&) = delete;

  ~ScopedNSAutoreleasePool();

  // Clear out the pool in case its position on the stack causes it to be
  // alive for long periods of time (such as the entire length of the app).
  // Only use then when you're certain the items currently in the pool are
  // no longer needed.
  void Recycle();

 private:
  void* autorelease_pool_;
#endif  // BUILDFLAG(IS_APPLE)
};

}  // namespace apple
}  // namespace base

#endif  // MINI_CHROMIUM_BASE_APPLE_SCOPED_NSAUTORELEASE_POOL_H_
