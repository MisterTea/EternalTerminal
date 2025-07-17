/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _LIBUNWINDSTACK_MEMORY_OFFLINE_BUFFER_H
#define _LIBUNWINDSTACK_MEMORY_OFFLINE_BUFFER_H

#include <stdint.h>

#include <unwindstack/Memory.h>

namespace unwindstack {

class MemoryOfflineBuffer : public Memory {
 public:
  MemoryOfflineBuffer(const uint8_t* data, uint64_t start, uint64_t end);
  virtual ~MemoryOfflineBuffer() = default;

  void Reset(const uint8_t* data, uint64_t start, uint64_t end);

  size_t Read(uint64_t addr, void* dst, size_t size) override;

 private:
  const uint8_t* data_;
  uint64_t start_;
  uint64_t end_;
};

}  // namespace unwindstack

#endif  // _LIBUNWINDSTACK_MEMORY_OFFLINE_BUFFER_H
