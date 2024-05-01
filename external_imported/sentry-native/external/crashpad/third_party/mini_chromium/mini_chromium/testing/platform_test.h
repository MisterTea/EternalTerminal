// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_TESTING_PLATFORM_TEST_H_
#define MINI_CHROMIUM_TESTING_PLATFORM_TEST_H_

#include "build/build_config.h"
#include "gtest/gtest.h"

#if BUILDFLAG(IS_APPLE)

// Note that this uses the direct runtime interface to the autorelease pool.
// https://clang.llvm.org/docs/AutomaticReferenceCounting.html#runtime-support
// This is so this can work when compiled for ARC.

extern "C" {
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);
}

// The implementation is in this header because mini_chromium does not directly
// depend on googletest. Consumers are free to use this interface if they do
// depend on googletest.
class PlatformTest : public testing::Test {
 public:
  PlatformTest(const PlatformTest&) = delete;
  PlatformTest& operator=(const PlatformTest&) = delete;

  ~PlatformTest() override { objc_autoreleasePoolPop(autorelease_pool_); }

 protected:
  PlatformTest() : autorelease_pool_(objc_autoreleasePoolPush()) {}

 private:
  void* autorelease_pool_;
};
#else
using PlatformTest = testing::Test;
#endif  // BUILDFLAG(IS_APPLE)

#endif  // MINI_CHROMIUM_TESTING_PLATFORM_TEST_H_
