// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_TESTING_PLATFORM_TEST_H_
#define MINI_CHROMIUM_TESTING_PLATFORM_TEST_H_

#include "base/macros.h"
#include "build/build_config.h"
#include "gtest/gtest.h"

#if defined(OS_APPLE)
#import <Foundation/Foundation.h>

// The implementation is in this header because mini_chromium does not directly
// depend on googletest. Consumers are free to use this interface if they do
// depend on googletest.
class PlatformTest : public testing::Test {
 public:
  ~PlatformTest() override { [pool_ release]; }

 protected:
  PlatformTest() : pool_([[NSAutoreleasePool alloc] init]) {}

 private:
#if !defined(__has_feature) || !__has_feature(objc_arc)
  using PoolType = NSAutoreleasePool*;
#else
  using PoolType = id;
#endif
  PoolType pool_;

  DISALLOW_COPY_AND_ASSIGN(PlatformTest);
};
#else
using PlatformTest = testing::Test;
#endif  // OS_APPLE

#endif  // MINI_CHROMIUM_TESTING_PLATFORM_TEST_H_
