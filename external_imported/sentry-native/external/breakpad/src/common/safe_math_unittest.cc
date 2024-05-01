// Copyright 2022 Google LLC
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

// safe_math_unittest.cc: Unit tests for SafeMath

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include "safe_math.h"
#include "breakpad_googletest_includes.h"

namespace {

using google_breakpad::AddIgnoringOverflow;
using google_breakpad::AddWithOverflowCheck;

TEST(SafeMath, AddOverflowWorksAsIntended) {
  EXPECT_EQ(AddWithOverflowCheck<uint8_t>(0, 0),
            std::make_pair<uint8_t>(0, false));
  EXPECT_EQ(AddWithOverflowCheck<uint8_t>(0, 255),
            std::make_pair<uint8_t>(255, false));
  EXPECT_EQ(AddWithOverflowCheck<uint8_t>(1, 255),
            std::make_pair<uint8_t>(0, true));

  EXPECT_EQ(AddWithOverflowCheck<int8_t>(-128, 127),
            std::make_pair<int8_t>(-1, false));
  EXPECT_EQ(AddWithOverflowCheck<int8_t>(127, -128),
            std::make_pair<int8_t>(-1, false));
  EXPECT_EQ(AddWithOverflowCheck<int8_t>(1, -128),
            std::make_pair<int8_t>(-127, false));
  EXPECT_EQ(AddWithOverflowCheck<int8_t>(127, -1),
            std::make_pair<int8_t>(126, false));

  EXPECT_EQ(AddWithOverflowCheck<int8_t>(-128, -1),
            std::make_pair<int8_t>(127, true));
  EXPECT_EQ(AddWithOverflowCheck<int8_t>(-128, -128),
            std::make_pair<int8_t>(0, true));
  EXPECT_EQ(AddWithOverflowCheck<int8_t>(127, 1),
            std::make_pair<int8_t>(-128, true));
  EXPECT_EQ(AddWithOverflowCheck<int8_t>(127, 127),
            std::make_pair<int8_t>(-2, true));
}

TEST(SafeMath, AddIgnoringOverflowWorksAsIntended) {
  EXPECT_EQ(AddIgnoringOverflow<uint8_t>(0, 0), 0);
  EXPECT_EQ(AddIgnoringOverflow<uint8_t>(0, 255), 255);
  EXPECT_EQ(AddIgnoringOverflow<uint8_t>(1, 255), 0);
}

}  // namespace
