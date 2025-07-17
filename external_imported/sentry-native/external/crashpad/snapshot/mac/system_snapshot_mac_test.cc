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

#include "snapshot/mac/system_snapshot_mac.h"

#include <sys/time.h>

#include <string>

#include "build/build_config.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "snapshot/mac/process_reader_mac.h"
#include "test/errors.h"
#include "util/mac/mac_util.h"

namespace crashpad {
namespace test {
namespace {

// SystemSnapshotMac objects would be cumbersome to construct in each test that
// requires one, because of the repetitive and mechanical work necessary to set
// up a ProcessReaderMac and timeval, along with the checks to verify that these
// operations succeed. This test fixture class handles the initialization work
// so that individual tests don’t have to.
class SystemSnapshotMacTest : public testing::Test {
 public:
  SystemSnapshotMacTest()
      : Test(),
        process_reader_(),
        snapshot_time_(),
        system_snapshot_() {
  }

  SystemSnapshotMacTest(const SystemSnapshotMacTest&) = delete;
  SystemSnapshotMacTest& operator=(const SystemSnapshotMacTest&) = delete;

  const internal::SystemSnapshotMac& system_snapshot() const {
    return system_snapshot_;
  }

  // testing::Test:
  void SetUp() override {
    ASSERT_TRUE(process_reader_.Initialize(mach_task_self()));
    ASSERT_EQ(gettimeofday(&snapshot_time_, nullptr), 0)
        << ErrnoMessage("gettimeofday");
    system_snapshot_.Initialize(&process_reader_, &snapshot_time_);
  }

 private:
  ProcessReaderMac process_reader_;
  timeval snapshot_time_;
  internal::SystemSnapshotMac system_snapshot_;
};

TEST_F(SystemSnapshotMacTest, GetCPUArchitecture) {
  CPUArchitecture cpu_architecture = system_snapshot().GetCPUArchitecture();

#if defined(ARCH_CPU_X86)
  EXPECT_EQ(cpu_architecture, kCPUArchitectureX86);
#elif defined(ARCH_CPU_X86_64)
  EXPECT_EQ(cpu_architecture, kCPUArchitectureX86_64);
#elif defined(ARCH_CPU_ARM64)
  EXPECT_EQ(cpu_architecture, kCPUArchitectureARM64);
#else
#error port to your architecture
#endif
}

TEST_F(SystemSnapshotMacTest, CPUCount) {
  EXPECT_GE(system_snapshot().CPUCount(), 1);
}

TEST_F(SystemSnapshotMacTest, CPUVendor) {
  std::string cpu_vendor = system_snapshot().CPUVendor();

#if defined(ARCH_CPU_X86_FAMILY)
  // Apple has only shipped Intel x86-family CPUs, but here’s a small nod to the
  // “Hackintosh” crowd.
  if (cpu_vendor != "GenuineIntel" && cpu_vendor != "AuthenticAMD") {
    FAIL() << "cpu_vendor " << cpu_vendor;
  }
#elif defined(ARCH_CPU_ARM64)
  EXPECT_THAT(cpu_vendor, testing::StartsWith("Apple "));
#else
#error port to your architecture
#endif
}

#if defined(ARCH_CPU_X86_FAMILY)

TEST_F(SystemSnapshotMacTest, CPUX86SupportsDAZ) {
  // All x86-family CPUs that Apple is known to have shipped should support DAZ.
  EXPECT_TRUE(system_snapshot().CPUX86SupportsDAZ());
}

#endif

TEST_F(SystemSnapshotMacTest, GetOperatingSystem) {
  EXPECT_EQ(system_snapshot().GetOperatingSystem(),
            SystemSnapshot::kOperatingSystemMacOSX);
}

TEST_F(SystemSnapshotMacTest, OSVersion) {
  int major;
  int minor;
  int bugfix;
  std::string build;
  system_snapshot().OSVersion(&major, &minor, &bugfix, &build);

  const int macos_version_number = MacOSVersionNumber();
  EXPECT_EQ(major * 1'00'00 + minor * 1'00 +
                (macos_version_number >= 10'13'04 ? bugfix : 0),
            macos_version_number);
  EXPECT_FALSE(build.empty());
}

TEST_F(SystemSnapshotMacTest, OSVersionFull) {
  EXPECT_FALSE(system_snapshot().OSVersionFull().empty());
}

TEST_F(SystemSnapshotMacTest, MachineDescription) {
  EXPECT_FALSE(system_snapshot().MachineDescription().empty());
}

TEST_F(SystemSnapshotMacTest, NXEnabled) {
  // Assume NX will always be enabled, as it was always enabled by default on
  // all supported versions of macOS.
  EXPECT_TRUE(system_snapshot().NXEnabled());
}

}  // namespace
}  // namespace test
}  // namespace crashpad
