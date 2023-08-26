// Copyright 2021 The Crashpad Authors
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

#include "util/ios/scoped_vm_read.h"

#include "util/ios/raw_logging.h"

namespace crashpad {
namespace internal {

ScopedVMReadInternal::ScopedVMReadInternal()
    : data_(0), region_start_(0), region_size_(0) {}

ScopedVMReadInternal::~ScopedVMReadInternal() {
  Reset();
}

bool ScopedVMReadInternal::Read(const void* data, const size_t data_length) {
  Reset();

  vm_address_t data_address = reinterpret_cast<vm_address_t>(data);
  vm_address_t page_region_address = trunc_page(data_address);
  vm_size_t page_region_size =
      round_page(data_address - page_region_address + data_length);
  if (page_region_size < data_length) {
    CRASHPAD_RAW_LOG("ScopedVMRead data_length overflow");
    return false;
  }
  kern_return_t kr = vm_read(mach_task_self(),
                             page_region_address,
                             page_region_size,
                             &region_start_,
                             &region_size_);

  if (kr != KERN_SUCCESS) {
    // It's expected that this will sometimes fail. Don't log here.
    return false;
  }

  data_ = region_start_ + (data_address - page_region_address);
  return true;
}

void ScopedVMReadInternal::Reset() {
  if (!region_start_) {
    return;
  }
  kern_return_t kr =
      vm_deallocate(mach_task_self(), region_start_, region_size_);
  if (kr != KERN_SUCCESS) {
    CRASHPAD_RAW_LOG_ERROR(kr, "vm_deallocate");
  }
  region_start_ = 0;
  region_size_ = 0;
  data_ = 0;
}

}  // namespace internal
}  // namespace crashpad
