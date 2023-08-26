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

#include "snapshot/mac/thread_snapshot_mac.h"

#include "base/check_op.h"
#include "snapshot/mac/cpu_context_mac.h"
#include "snapshot/mac/process_reader_mac.h"

#ifdef CLIENT_STACKTRACES_ENABLED
#include <libunwind.h>
#endif

namespace crashpad {
namespace internal {

ThreadSnapshotMac::ThreadSnapshotMac()
    : ThreadSnapshot(),
      context_union_(),
      context_(),
      stack_(),
      thread_name_(),
      thread_id_(0),
      thread_specific_data_address_(0),
      thread_(MACH_PORT_NULL),
      suspend_count_(0),
      priority_(0),
      initialized_() {}

ThreadSnapshotMac::~ThreadSnapshotMac() {}

bool ThreadSnapshotMac::Initialize(
    ProcessReaderMac* process_reader,
    const ProcessReaderMac::Thread& process_reader_thread) {
  INITIALIZATION_STATE_SET_INITIALIZING(initialized_);

  thread_ = process_reader_thread.port;
  thread_id_ = process_reader_thread.id;
  thread_name_ = process_reader_thread.name;
  suspend_count_ = process_reader_thread.suspend_count;
  priority_ = process_reader_thread.priority;
  thread_specific_data_address_ =
      process_reader_thread.thread_specific_data_address;

  stack_.Initialize(process_reader->Memory(),
                    process_reader_thread.stack_region_address,
                    process_reader_thread.stack_region_size);

#if defined(ARCH_CPU_X86_FAMILY)
  if (process_reader->Is64Bit()) {
    context_.architecture = kCPUArchitectureX86_64;
    context_.x86_64 = &context_union_.x86_64;
    InitializeCPUContextX86_64(context_.x86_64,
                               THREAD_STATE_NONE,
                               nullptr,
                               0,
                               &process_reader_thread.thread_context.t64,
                               &process_reader_thread.float_context.f64,
                               &process_reader_thread.debug_context.d64);
  } else {
    context_.architecture = kCPUArchitectureX86;
    context_.x86 = &context_union_.x86;
    InitializeCPUContextX86(context_.x86,
                            THREAD_STATE_NONE,
                            nullptr,
                            0,
                            &process_reader_thread.thread_context.t32,
                            &process_reader_thread.float_context.f32,
                            &process_reader_thread.debug_context.d32);
  }
#elif defined(ARCH_CPU_ARM64)
  context_.architecture = kCPUArchitectureARM64;
  context_.arm64 = &context_union_.arm64;
  InitializeCPUContextARM64(context_.arm64,
                            THREAD_STATE_NONE,
                            nullptr,
                            0,
                            &process_reader_thread.thread_context,
                            &process_reader_thread.float_context,
                            &process_reader_thread.debug_context);
#else
#error Port to your CPU architecture
#endif

#ifdef CLIENT_STACKTRACES_ENABLED
  unw_addr_space_t as = unw_create_addr_space_for_task(process_reader->task_);
  unw_cursor_t cursor;

  if (unw_init_remote_thread(&cursor, as, thread_) == UNW_ESUCCESS) {
    do {
      unw_word_t addr;
      unw_get_reg(&cursor, UNW_REG_IP, &addr);

      std::string sym("");
      char buf[1024];
      unw_word_t symbol_offset;
      if (unw_get_proc_name(&cursor, buf, sizeof(buf), &symbol_offset) ==
          UNW_ESUCCESS) {
        if (buf[0] == '_') {
          sym = std::string(buf + 1);
        } else {
          sym = std::string(buf);
        }
      }

      FrameSnapshot frame(addr, sym);
      frames_.push_back(frame);
    } while (unw_step(&cursor) > 0);
  }

  unw_destroy_addr_space(as);
#endif

  INITIALIZATION_STATE_SET_VALID(initialized_);
  return true;
}

const CPUContext* ThreadSnapshotMac::Context() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return &context_;
}

const MemorySnapshot* ThreadSnapshotMac::Stack() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return &stack_;
}

uint64_t ThreadSnapshotMac::ThreadID() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return thread_id_;
}

std::string ThreadSnapshotMac::ThreadName() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return thread_name_;
}

int ThreadSnapshotMac::SuspendCount() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return suspend_count_;
}

int ThreadSnapshotMac::Priority() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return priority_;
}

uint64_t ThreadSnapshotMac::ThreadSpecificDataAddress() const {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);
  return thread_specific_data_address_;
}

std::vector<const MemorySnapshot*> ThreadSnapshotMac::ExtraMemory() const {
  return std::vector<const MemorySnapshot*>();
}

}  // namespace internal
}  // namespace crashpad
