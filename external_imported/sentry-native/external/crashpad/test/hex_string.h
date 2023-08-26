// Copyright 2017 The Crashpad Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CRASHPAD_TEST_HEX_STRING_H_
#define CRASHPAD_TEST_HEX_STRING_H_

#include <sys/types.h>

#include <string>

namespace crashpad {
namespace test {

//! \brief Returns a hexadecimal string corresponding to \a bytes and \a length.
//!
//! Example usage:
//! \code
//!   uint8_t expected[10];
//!   uint8_t observed[10];
//!   // …
//!   EXPECT_EQ(BytesToHexString(observed, std::size(observed)),
//!             BytesToHexString(expected, std::size(expected)));
//! \endcode
std::string BytesToHexString(const void* bytes, size_t length);

}  // namespace test
}  // namespace crashpad

#endif  // CRASHPAD_TEST_HEX_STRING_H_
