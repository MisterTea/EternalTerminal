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

#ifndef CRASHPAD_UTIL_WIN_SCREENSHOT_H_
#define CRASHPAD_UTIL_WIN_SCREENSHOT_H_

#include "base/files/file_path.h"
#include "util/process/process_id.h"

namespace crashpad {

//! \brief Utility function for capturing screenshots on Windows.
//!
//! \param[in] process_id The process ID to capture the screenshot of.
//! \param[in] path The path to save the screenshot to.
//! \return `true` on success. `false` on failure with a message logged.
bool CaptureScreenshot(ProcessID process_id, const base::FilePath& path);

}  // namespace crashpad

#endif  // CRASHPAD_UTIL_WIN_SCREENSHOT_H_
