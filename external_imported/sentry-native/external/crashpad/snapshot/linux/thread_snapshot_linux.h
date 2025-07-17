// Copyright 2017 The Crashpad Authors
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

#ifndef CRASHPAD_SNAPSHOT_LINUX_THREAD_SNAPSHOT_LINUX_H_
#define CRASHPAD_SNAPSHOT_LINUX_THREAD_SNAPSHOT_LINUX_H_

#include <stdint.h>

#include "build/build_config.h"
#include "snapshot/cpu_context.h"
#include "snapshot/linux/process_reader_linux.h"
#include "snapshot/memory_snapshot.h"
#include "snapshot/memory_snapshot_generic.h"
#include "snapshot/thread_snapshot.h"
#include "util/misc/initialization_state_dcheck.h"

namespace crashpad {
namespace internal {

//! \brief A ThreadSnapshot of a thread on a Linux system.
class ThreadSnapshotLinux final : public ThreadSnapshot {
 public:
  ThreadSnapshotLinux();

  ThreadSnapshotLinux(const ThreadSnapshotLinux&) = delete;
  ThreadSnapshotLinux& operator=(const ThreadSnapshotLinux&) = delete;

  ~ThreadSnapshotLinux() override;

  //! \brief Initializes the object.
  //!
  //! \param[in] process_reader A ProcessReaderLinux for the process containing
  //!     the thread.
  //! \param[in] thread The thread within the ProcessReaderLinux for
  //!     which the snapshot should be created.
  //!
  //! \return `true` if the snapshot could be created, `false` otherwise with
  //!     a message logged.
  bool Initialize(
      ProcessReaderLinux* process_reader,
      const ProcessReaderLinux::Thread& thread,
      uint32_t* gather_indirectly_referenced_memory_bytes_remaining);

  // ThreadSnapshot:

  const CPUContext* Context() const override;
  const MemorySnapshot* Stack() const override;
  uint64_t ThreadID() const override;
  std::string ThreadName() const override;
  int SuspendCount() const override;
  int Priority() const override;
  uint64_t ThreadSpecificDataAddress() const override;
  std::vector<const MemorySnapshot*> ExtraMemory() const override;

#ifdef CLIENT_STACKTRACES_ENABLED
  void TrimStackTrace(uint64_t exception_address);
#endif

 private:
  union {
#if defined(ARCH_CPU_X86_FAMILY)
    CPUContextX86 x86;
    CPUContextX86_64 x86_64;
#elif defined(ARCH_CPU_ARM_FAMILY)
    CPUContextARM arm;
    CPUContextARM64 arm64;
#elif defined(ARCH_CPU_MIPS_FAMILY)
    CPUContextMIPS mipsel;
    CPUContextMIPS64 mips64;
#elif defined(ARCH_CPU_RISCV64)
    CPUContextRISCV64 riscv64;
#else
#error Port.
#endif  // ARCH_CPU_X86_FAMILY
  } context_union_;
  CPUContext context_;
  MemorySnapshotGeneric stack_;
  LinuxVMAddress thread_specific_data_address_;
  std::string thread_name_;
  pid_t thread_id_;
  int priority_;
  InitializationStateDcheck initialized_;
  std::vector<std::unique_ptr<MemorySnapshotGeneric>> pointed_to_memory_;
};

}  // namespace internal
}  // namespace crashpad

#endif  // CRASHPAD_SNAPSHOT_LINUX_THREAD_SNAPSHOT_LINUX_H_
