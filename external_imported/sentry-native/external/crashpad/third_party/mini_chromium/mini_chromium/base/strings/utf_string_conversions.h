// Copyright 2009 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_STRINGS_UTF_STRING_CONVERSIONS_H_
#define MINI_CHROMIUM_BASE_STRINGS_UTF_STRING_CONVERSIONS_H_

#include <string>
#include <string_view>

#include "build/build_config.h"

namespace base {

bool UTF8ToUTF16(const char* src, size_t src_len, std::u16string* output);
std::u16string UTF8ToUTF16(std::string_view utf8);
bool UTF16ToUTF8(const char16_t* src, size_t src_len, std::string* output);
std::string UTF16ToUTF8(std::u16string_view utf16);

#if defined(WCHAR_T_IS_16_BIT)
std::string WideToUTF8(std::wstring_view wide);
std::wstring UTF8ToWide(std::string_view utf8);
#endif  // defined(WCHAR_T_IS_16_BIT)

}  // namespace

#endif  // MINI_CHROMIUM_BASE_STRINGS_UTF_STRING_CONVERSIONS_H_
