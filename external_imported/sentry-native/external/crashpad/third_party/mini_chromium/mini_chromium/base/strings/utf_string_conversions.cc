// Copyright 2009 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/strings/utf_string_conversions.h"

#include <stdint.h>

#include <string>
#include <string_view>

#include "base/strings/utf_string_conversion_utils.h"
#include "build/build_config.h"

namespace {

template<typename SRC_CHAR, typename DEST_STRING>
bool ConvertUnicode(const SRC_CHAR* src,
                    size_t src_len,
                    DEST_STRING* output) {
  bool success = true;
  int32_t src_len32 = static_cast<int32_t>(src_len);
  for (int32_t i = 0; i < src_len32; i++) {
    uint32_t code_point;
    if (base::ReadUnicodeCharacter(src, src_len32, &i, &code_point)) {
      base::WriteUnicodeCharacter(code_point, output);
    } else {
      base::WriteUnicodeCharacter(0xFFFD, output);
      success = false;
    }
  }

  return success;
}

}  // namespace

namespace base {

bool UTF8ToUTF16(const char* src, size_t src_len, std::u16string* output) {
  base::PrepareForUTF16Or32Output(src, src_len, output);
  return ConvertUnicode(src, src_len, output);
}

std::u16string UTF8ToUTF16(std::string_view utf8) {
  std::u16string ret;
  UTF8ToUTF16(utf8.data(), utf8.length(), &ret);
  return ret;
}

bool UTF16ToUTF8(const char16_t* src, size_t src_len, std::string* output) {
  base::PrepareForUTF8Output(src, src_len, output);
  return ConvertUnicode(src, src_len, output);
}

std::string UTF16ToUTF8(std::u16string_view utf16) {
  std::string ret;
  UTF16ToUTF8(utf16.data(), utf16.length(), &ret);
  return ret;
}

#if defined(WCHAR_T_IS_16_BIT)
std::string WideToUTF8(std::wstring_view wide) {
  std::string ret;
  UTF16ToUTF8(
      reinterpret_cast<const char16_t*>(wide.data()), wide.size(), &ret);
  return ret;
}

std::wstring UTF8ToWide(std::string_view utf8) {
  std::u16string utf16 = UTF8ToUTF16(utf8);
  return std::wstring(reinterpret_cast<const wchar_t*>(utf16.data()),
                      utf16.size());
}
#endif  // defined(WCHAR_T_IS_16_BIT)

}  // namespace
