// Copyright 2015 The Crashpad Authors. All rights reserved.
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

#ifndef CRASHPAD_SNAPSHOT_WIN_SYSTEM_SNAPSHOT_WIN_H_
#define CRASHPAD_SNAPSHOT_WIN_SYSTEM_SNAPSHOT_WIN_H_

#include <stdint.h>
#include <sys/time.h>

#include <string>

#include "snapshot/system_snapshot.h"
#include "snapshot/win/process_reader_win.h"
#include "util/misc/initialization_state_dcheck.h"

namespace crashpad {

class ProcessReaderWin;

namespace internal {

//! \brief A SystemSnapshot of the running system, when the system runs Windows.
class SystemSnapshotWin final : public SystemSnapshot {
 public:
  SystemSnapshotWin();

  SystemSnapshotWin(const SystemSnapshotWin&) = delete;
  SystemSnapshotWin& operator=(const SystemSnapshotWin&) = delete;

  ~SystemSnapshotWin() override;

  //! \brief Initializes the object.
  //!
  //! \param[in] process_reader A reader for the process being snapshotted.
  //!
  //!     It seems odd that a system snapshot implementation would need a
  //!     ProcessReaderWin, but some of the information reported about the
  //!     system depends on the process it's being reported for. For example,
  //!     the architecture returned by GetCPUArchitecture() should be the
  //!     architecture of the process, which may be different than the native
  //!     architecture of the system: an x86_64 system can run both x86_64 and
  //!     32-bit x86 processes.
  void Initialize(ProcessReaderWin* process_reader);

  // SystemSnapshot:

  CPUArchitecture GetCPUArchitecture() const override;
  uint32_t CPURevision() const override;
  uint8_t CPUCount() const override;
  std::string CPUVendor() const override;
  void CPUFrequency(uint64_t* current_hz, uint64_t* max_hz) const override;
  uint32_t CPUX86Signature() const override;
  uint64_t CPUX86Features() const override;
  uint64_t CPUX86ExtendedFeatures() const override;
  uint32_t CPUX86Leaf7Features() const override;
  bool CPUX86SupportsDAZ() const override;
  OperatingSystem GetOperatingSystem() const override;
  bool OSServer() const override;
  void OSVersion(int* major,
                 int* minor,
                 int* bugfix,
                 std::string* build) const override;
  std::string OSVersionFull() const override;
  bool NXEnabled() const override;
  std::string MachineDescription() const override;
  void TimeZone(DaylightSavingTimeStatus* dst_status,
                int* standard_offset_seconds,
                int* daylight_offset_seconds,
                std::string* standard_name,
                std::string* daylight_name) const override;

 private:
  std::string os_version_full_;
  std::string os_version_build_;
  ProcessReaderWin* process_reader_;  // weak
  int os_version_major_;
  int os_version_minor_;
  int os_version_bugfix_;
  bool os_server_;
  InitializationStateDcheck initialized_;
};

}  // namespace internal
}  // namespace crashpad

#endif  // CRASHPAD_SNAPSHOT_WIN_SYSTEM_SNAPSHOT_WIN_H_
