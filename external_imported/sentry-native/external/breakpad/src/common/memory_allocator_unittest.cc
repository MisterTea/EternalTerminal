// Copyright 2009 Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <stdint.h>

#include <limits>

#include "breakpad_googletest_includes.h"
#include "common/memory_allocator.h"

using namespace google_breakpad;

namespace {
typedef testing::Test PageAllocatorTest;
}  // namespace

TEST(PageAllocatorTest, Setup) {
  PageAllocator allocator;
  EXPECT_EQ(0U, allocator.pages_allocated());
}

TEST(PageAllocatorTest, SmallObjects) {
  PageAllocator allocator;

  EXPECT_EQ(0U, allocator.pages_allocated());
  for (unsigned i = 1; i < 1024; ++i) {
    uint8_t* p = reinterpret_cast<uint8_t*>(allocator.Alloc(i));
    ASSERT_FALSE(p == nullptr);
    memset(p, 0, i);
  }
}

TEST(PageAllocatorTest, LargeObject) {
  PageAllocator allocator;

  EXPECT_EQ(0U, allocator.pages_allocated());
  uint8_t* p = reinterpret_cast<uint8_t*>(allocator.Alloc(10000));
  ASSERT_FALSE(p == nullptr);
  EXPECT_EQ(3U, allocator.pages_allocated());
  for (unsigned i = 1; i < 10; ++i) {
    uint8_t* p = reinterpret_cast<uint8_t*>(allocator.Alloc(i));
    ASSERT_FALSE(p == nullptr);
    memset(p, 0, i);
  }
}

TEST(PageAllocatorTest, AlignUp) {
  EXPECT_EQ(PageAllocator::AlignUp(0x11U, 1), 0x11U);
  EXPECT_EQ(PageAllocator::AlignUp(0x11U, 2), 0x12U);
  EXPECT_EQ(PageAllocator::AlignUp(0x13U, 2), 0x14U);
  EXPECT_EQ(PageAllocator::AlignUp(0x11U, 4), 0x14U);
  EXPECT_EQ(PageAllocator::AlignUp(0x15U, 4), 0x18U);
  EXPECT_EQ(PageAllocator::AlignUp(0x11U, 8), 0x18U);
  EXPECT_EQ(PageAllocator::AlignUp(0x19U, 8), 0x20U);

  // Ensure large 64 bit values are not truncated.
  constexpr uint64_t kUnalignedU64 = 0x8000'0000'0000'0011;
  constexpr uint64_t kAligned8U64 = 0x8000'0000'0000'0018;
  static_assert(kUnalignedU64 > std::numeric_limits<uint32_t>::max());
  static_assert(kAligned8U64 > std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(PageAllocator::AlignUp(kUnalignedU64, 8), kAligned8U64);
}

namespace {
typedef testing::Test PageAllocatorDeathTest;
}  // namespace

TEST(PageAllocatorDeathTest, AlignUpBad0) {
  EXPECT_DEBUG_DEATH({ PageAllocator::AlignUp(0x11U, 0); }, "");
}

TEST(PageAllocatorDeathTest, AlignUpBad9) {
  EXPECT_DEBUG_DEATH({ PageAllocator::AlignUp(0x11U, 9); }, "");
}

namespace {
typedef testing::Test WastefulVectorTest;
}

TEST(WastefulVectorTest, Setup) {
  PageAllocator allocator_;
  wasteful_vector<int> v(&allocator_);
  ASSERT_TRUE(v.empty());
  ASSERT_EQ(v.size(), 0u);
}

TEST(WastefulVectorTest, Simple) {
  PageAllocator allocator_;
  EXPECT_EQ(0U, allocator_.pages_allocated());
  wasteful_vector<unsigned> v(&allocator_);

  for (unsigned i = 0; i < 256; ++i) {
    v.push_back(i);
    ASSERT_EQ(i, v.back());
    ASSERT_EQ(&v.back(), &v[i]);
  }
  ASSERT_FALSE(v.empty());
  ASSERT_EQ(v.size(), 256u);
  EXPECT_EQ(1U, allocator_.pages_allocated());
  for (unsigned i = 0; i < 256; ++i)
    ASSERT_EQ(v[i], i);
}

TEST(WastefulVectorTest, UsesPageAllocator) {
  PageAllocator allocator_;
  wasteful_vector<unsigned> v(&allocator_);
  EXPECT_EQ(1U, allocator_.pages_allocated());

  v.push_back(1);
  ASSERT_TRUE(allocator_.OwnsPointer(&v[0]));
}

TEST(WastefulVectorTest, AutoWastefulVector) {
  PageAllocator allocator_;
  EXPECT_EQ(0U, allocator_.pages_allocated());

  auto_wasteful_vector<unsigned, 4> v(&allocator_);
  EXPECT_EQ(0U, allocator_.pages_allocated());

  v.push_back(1);
  EXPECT_EQ(0U, allocator_.pages_allocated());
  EXPECT_FALSE(allocator_.OwnsPointer(&v[0]));

  v.resize(4);
  EXPECT_EQ(0U, allocator_.pages_allocated());
  EXPECT_FALSE(allocator_.OwnsPointer(&v[0]));

  v.resize(10);
  EXPECT_EQ(1U, allocator_.pages_allocated());
  EXPECT_TRUE(allocator_.OwnsPointer(&v[0]));
}
