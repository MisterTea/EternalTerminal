// Copyright 2015 The Crashpad Authors
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

#include "util/misc/paths.h"

#include <windows.h>

#include <iterator>

#include "base/logging.h"

namespace crashpad {

// static
bool Paths::Executable(base::FilePath* path) {
  // follow the maximum path length documented here:
  // https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
  constexpr DWORD kMaxPathChars = 32768;
  std::wstring executable_path(kMaxPathChars, L'\0');

  const DWORD len =
      GetModuleFileName(nullptr, &executable_path[0], kMaxPathChars);

  if (len == 0) {
    PLOG(ERROR) << "GetModuleFileName";
    return false;
  }

  if (len >= kMaxPathChars) {
    LOG(ERROR) << "GetModuleFileName: path exceeds maximum length";
    return false;
  }

  executable_path.resize(len);
  *path = base::FilePath(executable_path);
  return true;
}

}  // namespace crashpad
