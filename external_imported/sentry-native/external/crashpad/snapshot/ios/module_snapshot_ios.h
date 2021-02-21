// Copyright 2020 The Crashpad Authors. All rights reserved.
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

#ifndef CRASHPAD_SNAPSHOT_IOS_MODULE_SNAPSHOT_IOS_H_
#define CRASHPAD_SNAPSHOT_IOS_MODULE_SNAPSHOT_IOS_H_

#include <mach-o/dyld_images.h>
#include <stdint.h>
#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

#include "base/macros.h"
#include "snapshot/crashpad_info_client_options.h"
#include "snapshot/module_snapshot.h"
#include "util/misc/initialization_state_dcheck.h"

namespace crashpad {
namespace internal {

//! \brief A ModuleSnapshot of a code module (binary image) loaded into a
//!     running (or crashed) process on an iOS system.
class ModuleSnapshotIOS final : public ModuleSnapshot {
 public:
  ModuleSnapshotIOS();
  ~ModuleSnapshotIOS() override;

  // TODO(justincohen): This function is temporary, and will be broken into two
  // parts.  One to do an in-process dump of all the relevant information, and
  // two to initialize the snapshot after the in-process dump is loaded.
  //! \brief Initializes the object.
  //!
  //! \param[in] image The mach-o image to be loaded.
  //!
  //! \return `true` if the snapshot could be created.
  bool Initialize(const dyld_image_info* image);

  // TODO(justincohen): This function is temporary, and will be broken into two
  // parts.  One to do an in-process dump of all the relevant information, and
  // two to initialize the snapshot after the in-process dump is loaded.
  //! \brief Initializes the object specifically for the dyld module.
  //!
  //! \param[in] images The structure containing the necessary dyld information.
  //!
  //! \return `true` if the snapshot could be created.
  bool InitializeDyld(const dyld_all_image_infos* images);

  //! \brief Returns options from the module’s CrashpadInfo structure.
  //!
  //! \param[out] options Options set in the module’s CrashpadInfo structure.
  void GetCrashpadOptions(CrashpadInfoClientOptions* options);

  static const dyld_all_image_infos* DyldAllImageInfo();

  // ModuleSnapshot:
  std::string Name() const override;
  uint64_t Address() const override;
  uint64_t Size() const override;
  time_t Timestamp() const override;
  void FileVersion(uint16_t* version_0,
                   uint16_t* version_1,
                   uint16_t* version_2,
                   uint16_t* version_3) const override;
  void SourceVersion(uint16_t* version_0,
                     uint16_t* version_1,
                     uint16_t* version_2,
                     uint16_t* version_3) const override;
  ModuleType GetModuleType() const override;
  void UUIDAndAge(UUID* uuid, uint32_t* age) const override;
  std::string DebugFileName() const override;
  std::vector<uint8_t> BuildID() const override;
  std::vector<std::string> AnnotationsVector() const override;
  std::map<std::string, std::string> AnnotationsSimpleMap() const override;
  std::vector<AnnotationSnapshot> AnnotationObjects() const override;
  std::set<CheckedRange<uint64_t>> ExtraMemoryRanges() const override;
  std::vector<const UserMinidumpStream*> CustomMinidumpStreams() const override;

 private:
  // Gather the the module information based off of a mach_header_64 |address_|.
  bool FinishInitialization();

  std::string name_;
  uint64_t address_;
  uint64_t size_;
  time_t timestamp_;
  uint32_t dylib_version_;
  uint64_t source_version_;
  uint32_t filetype_;
  UUID uuid_;
  InitializationStateDcheck initialized_;

  DISALLOW_COPY_AND_ASSIGN(ModuleSnapshotIOS);
};

}  // namespace internal
}  // namespace crashpad

#endif  // CRASHPAD_SNAPSHOT_IOS_MODULE_SNAPSHOT_IOS_H_
