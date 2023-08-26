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

#include "snapshot/mac/process_types.h"

#include <Availability.h>
#include <mach/mach.h>
#include <string.h>

#include <iterator>
#include <limits>
#include <vector>

#include "base/strings/stringprintf.h"
#include "build/build_config.h"
#include "gtest/gtest.h"
#include "snapshot/mac/process_types/internal.h"
#include "test/mac/dyld.h"
#include "util/mac/mac_util.h"
#include "util/misc/from_pointer_cast.h"
#include "util/misc/implicit_cast.h"

namespace crashpad {
namespace test {
namespace {

#define TEST_STRING(process_reader, self_view, proctype_view, field)        \
  do {                                                                      \
    if (self_view->field) {                                                 \
      std::string proctype_string;                                          \
      ASSERT_TRUE(process_reader.Memory()->ReadCString(proctype_view.field, \
                                                       &proctype_string));  \
      EXPECT_EQ(proctype_string, self_view->field);                         \
    }                                                                       \
  } while (false)

TEST(ProcessTypes, DyldImagesSelf) {
  // Get the in-process view of dyld_all_image_infos, and check it for sanity.
  const dyld_all_image_infos* self_image_infos = DyldGetAllImageInfos();
  const int macos_version_number = MacOSVersionNumber();

  if (macos_version_number >= 10'15'00) {
    EXPECT_GE(self_image_infos->version, 16u);
  } else if (macos_version_number >= 10'12'00) {
    EXPECT_GE(self_image_infos->version, 15u);
  } else if (macos_version_number >= 10'09'00) {
    EXPECT_GE(self_image_infos->version, 13u);
  } else if (macos_version_number >= 10'07'00) {
    EXPECT_GE(self_image_infos->version, 8u);
  } else if (macos_version_number >= 10'06'00) {
    EXPECT_GE(self_image_infos->version, 2u);
  } else {
    EXPECT_GE(self_image_infos->version, 1u);
  }

  EXPECT_GT(self_image_infos->infoArrayCount, 1u);
  if (self_image_infos->version >= 2) {
    EXPECT_TRUE(self_image_infos->libSystemInitialized);
  }
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_7
  if (self_image_infos->version >= 9) {
    EXPECT_EQ(self_image_infos->dyldAllImageInfosAddress, self_image_infos);
  }
#endif

  // Get the out-of-process view of dyld_all_image_infos, and work with it
  // through the process_types interface.
  task_dyld_info_data_t dyld_info;
  mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
  kern_return_t kr = task_info(mach_task_self(),
                               TASK_DYLD_INFO,
                               reinterpret_cast<task_info_t>(&dyld_info),
                               &count);
  ASSERT_EQ(kr, KERN_SUCCESS);

  EXPECT_EQ(dyld_info.all_image_info_addr,
            FromPointerCast<mach_vm_address_t>(self_image_infos));
  EXPECT_GT(dyld_info.all_image_info_size, 1u);

  // This field is only present in the OS X 10.7 SDK (at build time) and kernel
  // (at run time).
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_7
  if (macos_version_number >= 10'07'00) {
#if !defined(ARCH_CPU_64_BITS)
    EXPECT_EQ(dyld_info.all_image_info_format, TASK_DYLD_ALL_IMAGE_INFO_32);
#else
    EXPECT_EQ(dyld_info.all_image_info_format, TASK_DYLD_ALL_IMAGE_INFO_64);
#endif
  }
#endif

  ProcessReaderMac process_reader;
  ASSERT_TRUE(process_reader.Initialize(mach_task_self()));

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_16
  constexpr uint32_t kDyldAllImageInfosVersionInSDK = 17;
#elif __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_15
  constexpr uint32_t kDyldAllImageInfosVersionInSDK = 16;
#elif __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_12
  constexpr uint32_t kDyldAllImageInfosVersionInSDK = 15;
#elif __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_9
  constexpr uint32_t kDyldAllImageInfosVersionInSDK = 14;
#elif __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_7
  constexpr uint32_t kDyldAllImageInfosVersionInSDK = 12;
#elif __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_6
  constexpr uint32_t kDyldAllImageInfosVersionInSDK = 7;
#else
  constexpr uint32_t kDyldAllImageInfosVersionInSDK = 1;
#endif

  // Make sure that the size of the structure as declared in the SDK matches the
  // size expected for the version of the structure that the SDK describes.
  //
  // There are two possible layouts for version 15, and the
  // ExpectedSizeForVersion() implementation infers the correct one based on the
  // run-time OS version, so if the SDK defines the version 15 structure, this
  // test can only be performed if the run-time OS natively uses the same format
  // structure as the SDK.
  bool test_expected_size_for_version_matches_sdk_sizeof;
#if __MAC_OS_X_VERSION_MAX_ALLOWED == __MAC_10_12
  test_expected_size_for_version_matches_sdk_sizeof =
      macos_version_number / 1'00 == 10'12;
#elif __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_13 && \
    __MAC_OS_X_VERSION_MAX_ALLOWED < __MAC_10_15
  test_expected_size_for_version_matches_sdk_sizeof =
      macos_version_number >= 10'13'00 && macos_version_number < 10'15'00;
#else
  test_expected_size_for_version_matches_sdk_sizeof = true;
#endif

  if (test_expected_size_for_version_matches_sdk_sizeof) {
    EXPECT_EQ(process_types::dyld_all_image_infos::ExpectedSizeForVersion(
                  &process_reader, kDyldAllImageInfosVersionInSDK),
              sizeof(dyld_all_image_infos));
  }

  // Make sure that the computed sizes of various versions of this structure are
  // correct at different bitnessses. Version 16 and later are unsupported on
  // 32-bit systems due to the OS deprecating 32-bit support in macOS 10.15.
  constexpr size_t kSpecialCase = std::numeric_limits<size_t>::max();
  constexpr size_t kUnsupported = std::numeric_limits<size_t>::max() - 1;
  constexpr struct {
    uint32_t version;
    size_t size_32;
    size_t size_64;
  } kVersionsAndSizes[] = {
      {1, 17, 25},
      {2, 24, 40},
      {3, 28, 48},
      {5, 40, 72},
      {6, 44, 80},
      {7, 48, 88},
      {8, 56, 104},
      {9, 60, 112},
      {10, 64, 120},
      {11, 80, 152},
      {12, 84, 160},
      {13, 104, 184},
      {14, 164, 304},
      {15, kSpecialCase, kSpecialCase},
      {16, kUnsupported, 328},
      {17, kUnsupported, 368},
  };
  for (size_t index = 0; index < std::size(kVersionsAndSizes); ++index) {
    uint32_t version = kVersionsAndSizes[index].version;
    SCOPED_TRACE(base::StringPrintf("index %zu, version %u", index, version));

    if (version == 15) {
      if (macos_version_number / 1'00 == 10'12) {
        EXPECT_EQ(process_types::internal::dyld_all_image_infos<
                      process_types::internal::Traits32>::
                      ExpectedSizeForVersion(version),
                  164u);
        EXPECT_EQ(process_types::internal::dyld_all_image_infos<
                      process_types::internal::Traits64>::
                      ExpectedSizeForVersion(version),
                  304u);
      } else if (macos_version_number >= 10'13'00 &&
                 macos_version_number < 10'15'00) {
        EXPECT_EQ(process_types::internal::dyld_all_image_infos<
                      process_types::internal::Traits32>::
                      ExpectedSizeForVersion(version),
                  176u);
        EXPECT_EQ(process_types::internal::dyld_all_image_infos<
                      process_types::internal::Traits64>::
                      ExpectedSizeForVersion(version),
                  320u);
      }

      continue;
    }

    ASSERT_NE(kVersionsAndSizes[index].size_32, kSpecialCase);
    ASSERT_NE(kVersionsAndSizes[index].size_64, kSpecialCase);

    if (kVersionsAndSizes[index].size_32 != kUnsupported) {
      EXPECT_EQ(process_types::internal::dyld_all_image_infos<
                    process_types::internal::Traits32>::
                    ExpectedSizeForVersion(version),
                kVersionsAndSizes[index].size_32);
    }
    if (kVersionsAndSizes[index].size_64 != kUnsupported) {
      EXPECT_EQ(process_types::internal::dyld_all_image_infos<
                    process_types::internal::Traits64>::
                    ExpectedSizeForVersion(version),
                kVersionsAndSizes[index].size_64);
    }
  }

  process_types::dyld_all_image_infos proctype_image_infos;
  ASSERT_TRUE(proctype_image_infos.Read(&process_reader,
                                        dyld_info.all_image_info_addr));

  ASSERT_EQ(proctype_image_infos.version, self_image_infos->version);

  if (proctype_image_infos.version >= 1) {
    EXPECT_EQ(proctype_image_infos.infoArrayCount,
              self_image_infos->infoArrayCount);
    EXPECT_EQ(proctype_image_infos.infoArray,
              reinterpret_cast<uint64_t>(self_image_infos->infoArray));
    EXPECT_EQ(proctype_image_infos.notification,
              reinterpret_cast<uint64_t>(self_image_infos->notification));
    EXPECT_EQ(proctype_image_infos.processDetachedFromSharedRegion,
              self_image_infos->processDetachedFromSharedRegion);
  }
  if (proctype_image_infos.version >= 2) {
    EXPECT_EQ(proctype_image_infos.libSystemInitialized,
              self_image_infos->libSystemInitialized);
    EXPECT_EQ(
        proctype_image_infos.dyldImageLoadAddress,
        reinterpret_cast<uint64_t>(self_image_infos->dyldImageLoadAddress));
  }
  if (proctype_image_infos.version >= 3) {
    EXPECT_EQ(proctype_image_infos.jitInfo,
              reinterpret_cast<uint64_t>(self_image_infos->jitInfo));
  }
  if (proctype_image_infos.version >= 5) {
    EXPECT_EQ(proctype_image_infos.dyldVersion,
              reinterpret_cast<uint64_t>(self_image_infos->dyldVersion));
    EXPECT_EQ(proctype_image_infos.errorMessage,
              reinterpret_cast<uint64_t>(self_image_infos->errorMessage));
    EXPECT_EQ(proctype_image_infos.terminationFlags,
              implicit_cast<uint64_t>(self_image_infos->terminationFlags));

    TEST_STRING(
        process_reader, self_image_infos, proctype_image_infos, dyldVersion);
    TEST_STRING(
        process_reader, self_image_infos, proctype_image_infos, errorMessage);
  }
  if (proctype_image_infos.version >= 6) {
    EXPECT_EQ(
        proctype_image_infos.coreSymbolicationShmPage,
        reinterpret_cast<uint64_t>(self_image_infos->coreSymbolicationShmPage));
  }
  if (proctype_image_infos.version >= 7) {
    EXPECT_EQ(proctype_image_infos.systemOrderFlag,
              implicit_cast<uint64_t>(self_image_infos->systemOrderFlag));
  }
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_7
  if (proctype_image_infos.version >= 8) {
    EXPECT_EQ(proctype_image_infos.uuidArrayCount,
              implicit_cast<uint64_t>(self_image_infos->uuidArrayCount));
  }
  if (proctype_image_infos.version >= 9) {
    EXPECT_EQ(
        proctype_image_infos.dyldAllImageInfosAddress,
        reinterpret_cast<uint64_t>(self_image_infos->dyldAllImageInfosAddress));
  }
  if (proctype_image_infos.version >= 10) {
    EXPECT_EQ(proctype_image_infos.initialImageCount,
              implicit_cast<uint64_t>(self_image_infos->initialImageCount));
  }
  if (proctype_image_infos.version >= 11) {
    EXPECT_EQ(proctype_image_infos.errorKind,
              implicit_cast<uint64_t>(self_image_infos->errorKind));
    EXPECT_EQ(
        proctype_image_infos.errorClientOfDylibPath,
        reinterpret_cast<uint64_t>(self_image_infos->errorClientOfDylibPath));
    EXPECT_EQ(
        proctype_image_infos.errorTargetDylibPath,
        reinterpret_cast<uint64_t>(self_image_infos->errorTargetDylibPath));
    EXPECT_EQ(proctype_image_infos.errorSymbol,
              reinterpret_cast<uint64_t>(self_image_infos->errorSymbol));

    TEST_STRING(process_reader,
                self_image_infos,
                proctype_image_infos,
                errorClientOfDylibPath);
    TEST_STRING(process_reader,
                self_image_infos,
                proctype_image_infos,
                errorTargetDylibPath);
    TEST_STRING(
        process_reader, self_image_infos, proctype_image_infos, errorSymbol);
  }
  if (proctype_image_infos.version >= 12) {
    EXPECT_EQ(proctype_image_infos.sharedCacheSlide,
              implicit_cast<uint64_t>(self_image_infos->sharedCacheSlide));
  }
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_9
  if (proctype_image_infos.version >= 13) {
    EXPECT_EQ(memcmp(self_image_infos->sharedCacheUUID,
                     proctype_image_infos.sharedCacheUUID,
                     sizeof(self_image_infos->sharedCacheUUID)),
              0);
  }
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_12
  if (proctype_image_infos.version >= 15) {
    EXPECT_EQ(proctype_image_infos.infoArrayChangeTimestamp,
              self_image_infos->infoArrayChangeTimestamp);
    EXPECT_EQ(proctype_image_infos.sharedCacheBaseAddress,
              self_image_infos->sharedCacheBaseAddress);
    EXPECT_EQ(proctype_image_infos.dyldPath,
              reinterpret_cast<uint64_t>(self_image_infos->dyldPath));
    for (size_t index = 0; index < std::size(self_image_infos->notifyPorts);
         ++index) {
      EXPECT_EQ(proctype_image_infos.notifyPorts[index],
                self_image_infos->notifyPorts[index])
          << "index " << index;
    }

    TEST_STRING(
        process_reader, self_image_infos, proctype_image_infos, dyldPath);
  }
#endif

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_12
  // As dyld_all_image_infos has evolved over time, new fields were added to the
  // reserved region. process_types::dyld_all_image_infos declares a recent
  // version of the structure, but an older SDK may declare an older version
  // whose |reserved| member appears at a different (smaller) offset than the
  // process_types version. It’s difficult to compare the reserved fields in
  // these older SDKs, so only do it where the declarations match.
  if (proctype_image_infos.version >= 14) {
    for (size_t index = 0; index < std::size(proctype_image_infos.reserved);
         ++index) {
      EXPECT_EQ(proctype_image_infos.reserved[index],
                implicit_cast<uint64_t>(self_image_infos->reserved[index]))
          << "index " << index;
    }
#if defined(ARCH_CPU_64_BITS)
    EXPECT_EQ(proctype_image_infos.reserved_4_64,
              self_image_infos->reserved[4]);
    EXPECT_EQ(proctype_image_infos.reserved_5, self_image_infos->reserved[5]);
    EXPECT_EQ(proctype_image_infos.reserved_6, self_image_infos->reserved[6]);
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_12_0
    uint64_t shared_cache_fs_id = self_image_infos->sharedCacheFSID;
    uint64_t shared_cache_fs_obj_id = self_image_infos->sharedCacheFSObjID;
#else
    uint64_t shared_cache_fs_id = self_image_infos->reserved[7];
    uint64_t shared_cache_fs_obj_id = self_image_infos->reserved[8];
#endif
    EXPECT_EQ(proctype_image_infos.shared_cache_fs_id, shared_cache_fs_id);
    EXPECT_EQ(proctype_image_infos.shared_cache_fs_obj_id,
              shared_cache_fs_obj_id);
#endif
  }
#endif

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_13
  if (proctype_image_infos.version >= 15 && macos_version_number >= 10'13'00) {
    EXPECT_EQ(proctype_image_infos.compact_dyld_image_info_addr,
              self_image_infos->compact_dyld_image_info_addr);
    EXPECT_EQ(proctype_image_infos.compact_dyld_image_info_size,
              self_image_infos->compact_dyld_image_info_size);
  }
#endif

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_15
  if (proctype_image_infos.version >= 16) {
    EXPECT_EQ(proctype_image_infos.platform, self_image_infos->platform);
  }
#endif

  if (proctype_image_infos.version >= 1) {
    std::vector<process_types::dyld_image_info> proctype_image_info_vector(
        proctype_image_infos.infoArrayCount);
    ASSERT_TRUE(process_types::dyld_image_info::ReadArrayInto(
        &process_reader,
        proctype_image_infos.infoArray,
        proctype_image_info_vector.size(),
        &proctype_image_info_vector[0]));

    for (size_t index = 0;
         index < proctype_image_infos.infoArrayCount;
         ++index) {
      const dyld_image_info* self_image_info =
          &self_image_infos->infoArray[index];
      const process_types::dyld_image_info& proctype_image_info =
          proctype_image_info_vector[index];

      EXPECT_EQ(proctype_image_info.imageLoadAddress,
                reinterpret_cast<uint64_t>(self_image_info->imageLoadAddress))
          << "index " << index;
      EXPECT_EQ(proctype_image_info.imageFilePath,
                reinterpret_cast<uint64_t>(self_image_info->imageFilePath))
          << "index " << index;
      EXPECT_EQ(proctype_image_info.imageFileModDate,
                implicit_cast<uint64_t>(self_image_info->imageFileModDate))
          << "index " << index;

      TEST_STRING(
          process_reader, self_image_info, proctype_image_info, imageFilePath);
    }
  }

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_10_7
  if (proctype_image_infos.version >= 8) {
    std::vector<process_types::dyld_uuid_info> proctype_uuid_info_vector(
        proctype_image_infos.uuidArrayCount);
    ASSERT_TRUE(process_types::dyld_uuid_info::ReadArrayInto(
        &process_reader,
        proctype_image_infos.uuidArray,
        proctype_uuid_info_vector.size(),
        &proctype_uuid_info_vector[0]));

    for (size_t index = 0;
         index < proctype_image_infos.uuidArrayCount;
         ++index) {
      const dyld_uuid_info* self_uuid_info =
          &self_image_infos->uuidArray[index];
      const process_types::dyld_uuid_info& proctype_uuid_info =
          proctype_uuid_info_vector[index];

      EXPECT_EQ(proctype_uuid_info.imageLoadAddress,
                reinterpret_cast<uint64_t>(self_uuid_info->imageLoadAddress))
          << "index " << index;
      EXPECT_EQ(memcmp(self_uuid_info->imageUUID,
                       proctype_uuid_info.imageUUID,
                       sizeof(self_uuid_info->imageUUID)),
                0)
          << "index " << index;
    }
  }
#endif
}

}  // namespace
}  // namespace test
}  // namespace crashpad
