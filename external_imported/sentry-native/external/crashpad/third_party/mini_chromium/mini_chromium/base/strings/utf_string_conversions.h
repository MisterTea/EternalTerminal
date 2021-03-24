// Copyright 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_STRINGS_UTF_STRING_CONVERSIONS_H_
#define MINI_CHROMIUM_BASE_STRINGS_UTF_STRING_CONVERSIONS_H_

#include <string>

#include "base/strings/string_piece.h"
#include "build/build_config.h"

namespace base {

bool UTF8ToUTF16(const char* src, size_t src_len, std::u16string* output);
std::u16string UTF8ToUTF16(const StringPiece& utf8);
bool UTF16ToUTF8(const char16_t* src, size_t src_len, std::string* output);
std::string UTF16ToUTF8(const StringPiece16& utf16);

#if defined(WCHAR_T_IS_UTF16)
std::string WideToUTF8(WStringPiece wide);
std::wstring UTF8ToWide(StringPiece utf8);
#endif  // defined(WCHAR_T_IS_UTF16)

}  // namespace

#endif  // MINI_CHROMIUM_BASE_STRINGS_UTF_STRING_CONVERSIONS_H_
