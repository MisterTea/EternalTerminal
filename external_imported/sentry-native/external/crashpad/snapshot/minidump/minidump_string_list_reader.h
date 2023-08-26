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

#ifndef CRASHPAD_SNAPSHOT_MINIDUMP_MINIDUMP_STRING_LIST_READER_H_
#define CRASHPAD_SNAPSHOT_MINIDUMP_MINIDUMP_STRING_LIST_READER_H_

#include <windows.h>
#include <dbghelp.h>

#include <string>
#include <vector>

#include "util/file/file_reader.h"

namespace crashpad {
namespace internal {

//! \brief Reads a list of MinidumpUTF8String objects in a MinidumpRVAList from
//!     a minidump file \a location in \a file_reader, and returns it in \a
//!     list.
//!
//! \return `true` on success, with \a list set by replacing its contents.
//!     `false` on failure, with a message logged.
bool ReadMinidumpStringList(FileReaderInterface* file_reader,
                            const MINIDUMP_LOCATION_DESCRIPTOR& location,
                            std::vector<std::string>* list);

}  // namespace internal
}  // namespace crashpad

#endif  // CRASHPAD_SNAPSHOT_MINIDUMP_MINIDUMP_STRING_LIST_READER_H_
