// Copyright 2010 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_
#define MINI_CHROMIUM_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace base {

bool StringToInt(std::string_view input, int* output);
bool StringToUint(std::string_view input, unsigned int* output);
bool StringToInt64(std::string_view input, int64_t* output);
bool StringToUint64(std::string_view input, uint64_t* output);
bool StringToSizeT(std::string_view input, size_t* output);

bool HexStringToInt(std::string_view input, int* output);
bool HexStringToBytes(const std::string& input, std::vector<uint8_t>* output);

}  // namespace base

#endif  // MINI_CHROMIUM_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_
