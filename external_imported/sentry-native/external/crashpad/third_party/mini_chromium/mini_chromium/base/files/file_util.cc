// Copyright 2006-2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_util.h"

#if BUILDFLAG(IS_WIN)
#include "base/strings/utf_string_conversions.h"
#endif

namespace base {

FILE* OpenFile(const FilePath& filename, const char* mode) {
#if BUILDFLAG(IS_WIN)
  return _wfopen(filename.value().c_str(), UTF8ToWide(mode).c_str());
#else
  return fopen(filename.value().c_str(), mode);
#endif
}

}  // namespace base
