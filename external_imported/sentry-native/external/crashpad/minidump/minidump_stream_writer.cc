// Copyright 2014 The Crashpad Authors
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

#include "minidump/minidump_stream_writer.h"

#include "base/check_op.h"

namespace crashpad {
namespace internal {

MinidumpStreamWriter::~MinidumpStreamWriter() {
}

const MINIDUMP_DIRECTORY* MinidumpStreamWriter::DirectoryListEntry() const {
  DCHECK_EQ(state(), kStateWritable);

  return &directory_list_entry_;
}

MinidumpStreamWriter::MinidumpStreamWriter()
    : MinidumpWritable(), directory_list_entry_() {
}

bool MinidumpStreamWriter::Freeze() {
  DCHECK_EQ(state(), kStateMutable);

  if (!MinidumpWritable::Freeze()) {
    return false;
  }

  directory_list_entry_.StreamType = StreamType();
  RegisterLocationDescriptor(&directory_list_entry_.Location);

  return true;
}

}  // namespace internal
}  // namespace crashpad
