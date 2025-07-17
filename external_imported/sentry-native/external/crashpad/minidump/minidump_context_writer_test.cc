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

#include "minidump/minidump_context_writer.h"

#include <stdint.h>

#include <string>
#include <type_traits>

#include "base/notreached.h"
#include "gtest/gtest.h"
#include "minidump/minidump_context.h"
#include "minidump/test/minidump_context_test_util.h"
#include "minidump/test/minidump_writable_test_util.h"
#include "snapshot/cpu_context.h"
#include "snapshot/test/test_cpu_context.h"
#include "util/file/string_file.h"

namespace crashpad {
namespace test {
namespace {

template <typename Writer, typename Context, typename RVAType>
void EmptyContextTest(void (*expect_context)(uint32_t, const Context*, bool)) {
  Writer context_writer;
  StringFile string_file;
  EXPECT_TRUE(context_writer.WriteEverything(&string_file));
  ASSERT_EQ(string_file.string().size(), sizeof(Context));

  const Context* observed =
      MinidumpWritableAtRVA<Context>(string_file.string(), RVAType(0));
  ASSERT_TRUE(observed);

  expect_context(0, observed, false);
}

class TestTypeNames {
 public:
  template <typename T>
  static std::string GetName(int) {
    if (std::is_same<T, RVA>()) {
      return "RVA";
    }
    if (std::is_same<T, RVA64>()) {
      return "RVA64";
    }
    NOTREACHED();
    return "";
  }
};

template <typename RVAType>
class MinidumpContextWriter : public ::testing::Test {};

using RVATypes = ::testing::Types<RVA, RVA64>;
TYPED_TEST_SUITE(MinidumpContextWriter, RVATypes, TestTypeNames);

TYPED_TEST(MinidumpContextWriter, MinidumpContextX86Writer) {
  StringFile string_file;

  {
    // Make sure that a context writer that’s untouched writes a zeroed-out
    // context.
    SCOPED_TRACE("zero");

    EmptyContextTest<MinidumpContextX86Writer, MinidumpContextX86, TypeParam>(
        ExpectMinidumpContextX86);
  }

  {
    SCOPED_TRACE("nonzero");

    string_file.Reset();
    constexpr uint32_t kSeed = 0x8086;

    MinidumpContextX86Writer context_writer;
    InitializeMinidumpContextX86(context_writer.context(), kSeed);

    EXPECT_TRUE(context_writer.WriteEverything(&string_file));
    ASSERT_EQ(string_file.string().size(), sizeof(MinidumpContextX86));

    const MinidumpContextX86* observed =
        MinidumpWritableAtRVA<MinidumpContextX86>(string_file.string(),
                                                  TypeParam(0));
    ASSERT_TRUE(observed);

    ExpectMinidumpContextX86(kSeed, observed, false);
  }
}

TYPED_TEST(MinidumpContextWriter, MinidumpContextAMD64Writer) {
  {
    // Make sure that a heap-allocated context writer has the proper alignment,
    // because it may be nonstandard.
    auto context_writer = std::make_unique<MinidumpContextAMD64Writer>();
    EXPECT_EQ(reinterpret_cast<uintptr_t>(context_writer.get()) &
                  (alignof(MinidumpContextAMD64Writer) - 1),
              0u);
  }

  StringFile string_file;

  {
    // Make sure that a context writer that’s untouched writes a zeroed-out
    // context.
    SCOPED_TRACE("zero");

    EmptyContextTest<MinidumpContextAMD64Writer,
                     MinidumpContextAMD64,
                     TypeParam>(ExpectMinidumpContextAMD64);
  }

  {
    SCOPED_TRACE("nonzero");

    string_file.Reset();
    constexpr uint32_t kSeed = 0x808664;

    MinidumpContextAMD64Writer context_writer;
    InitializeMinidumpContextAMD64(context_writer.context(), kSeed);

    EXPECT_TRUE(context_writer.WriteEverything(&string_file));
    ASSERT_EQ(string_file.string().size(), sizeof(MinidumpContextAMD64));

    const MinidumpContextAMD64* observed =
        MinidumpWritableAtRVA<MinidumpContextAMD64>(string_file.string(),
                                                    TypeParam(0));
    ASSERT_TRUE(observed);

    ExpectMinidumpContextAMD64(kSeed, observed, false);
  }
}

template <typename Writer, typename Context, typename RVAType>
void FromSnapshotTest(const CPUContext& snapshot_context,
                      void (*expect_context)(uint32_t, const Context*, bool),
                      uint32_t seed) {
  std::unique_ptr<::crashpad::MinidumpContextWriter> context_writer =
      ::crashpad::MinidumpContextWriter::CreateFromSnapshot(&snapshot_context);
  ASSERT_TRUE(context_writer);

  StringFile string_file;
  ASSERT_TRUE(context_writer->WriteEverything(&string_file));

  const Context* observed =
      MinidumpWritableAtRVA<Context>(string_file.string(), RVAType(0));
  ASSERT_TRUE(observed);

  expect_context(seed, observed, true);
}

TYPED_TEST(MinidumpContextWriter, X86_FromSnapshot) {
  constexpr uint32_t kSeed = 32;
  CPUContextX86 context_x86;
  CPUContext context;
  context.x86 = &context_x86;
  InitializeCPUContextX86(&context, kSeed);
  FromSnapshotTest<MinidumpContextX86Writer, MinidumpContextX86, TypeParam>(
      context, ExpectMinidumpContextX86, kSeed);
}

TYPED_TEST(MinidumpContextWriter, AMD64_FromSnapshot) {
  constexpr uint32_t kSeed = 64;
  CPUContextX86_64 context_x86_64;
  CPUContext context;
  context.x86_64 = &context_x86_64;
  InitializeCPUContextX86_64(&context, kSeed);
  FromSnapshotTest<MinidumpContextAMD64Writer, MinidumpContextAMD64, TypeParam>(
      context, ExpectMinidumpContextAMD64, kSeed);
}

TYPED_TEST(MinidumpContextWriter, AMD64_CetFromSnapshot) {
  constexpr uint32_t kSeed = 77;
  CPUContextX86_64 context_x86_64;
  CPUContext context;
  context.x86_64 = &context_x86_64;
  InitializeCPUContextX86_64(&context, kSeed);
  context_x86_64.xstate.enabled_features |= XSTATE_MASK_CET_U;
  context_x86_64.xstate.cet_u.cetmsr = 1;
  context_x86_64.xstate.cet_u.ssp = kSeed * kSeed;
  // We cannot use FromSnapshotTest as we write more than the fixed context.
  std::unique_ptr<::crashpad::MinidumpContextWriter> context_writer =
      ::crashpad::MinidumpContextWriter::CreateFromSnapshot(&context);
  ASSERT_TRUE(context_writer);

  StringFile string_file;
  ASSERT_TRUE(context_writer->WriteEverything(&string_file));

  const MinidumpContextAMD64* observed =
      MinidumpWritableAtRVA<MinidumpContextAMD64>(string_file.string(),
                                                  TypeParam(0));
  ASSERT_TRUE(observed);

  ExpectMinidumpContextAMD64(kSeed, observed, true);
}

TYPED_TEST(MinidumpContextWriter, ARM_Zeros) {
  EmptyContextTest<MinidumpContextARMWriter, MinidumpContextARM, TypeParam>(
      ExpectMinidumpContextARM);
}

TYPED_TEST(MinidumpContextWriter, ARM64_Zeros) {
  EmptyContextTest<MinidumpContextARM64Writer, MinidumpContextARM64, TypeParam>(
      ExpectMinidumpContextARM64);
}

TYPED_TEST(MinidumpContextWriter, ARM_FromSnapshot) {
  constexpr uint32_t kSeed = 32;
  CPUContextARM context_arm;
  CPUContext context;
  context.arm = &context_arm;
  InitializeCPUContextARM(&context, kSeed);
  FromSnapshotTest<MinidumpContextARMWriter, MinidumpContextARM, TypeParam>(
      context, ExpectMinidumpContextARM, kSeed);
}

TYPED_TEST(MinidumpContextWriter, ARM64_FromSnapshot) {
  constexpr uint32_t kSeed = 64;
  CPUContextARM64 context_arm64;
  CPUContext context;
  context.arm64 = &context_arm64;
  InitializeCPUContextARM64(&context, kSeed);
  FromSnapshotTest<MinidumpContextARM64Writer, MinidumpContextARM64, TypeParam>(
      context, ExpectMinidumpContextARM64, kSeed);
}

TYPED_TEST(MinidumpContextWriter, MIPS_Zeros) {
  EmptyContextTest<MinidumpContextMIPSWriter, MinidumpContextMIPS, TypeParam>(
      ExpectMinidumpContextMIPS);
}

TYPED_TEST(MinidumpContextWriter, MIPS64_Zeros) {
  EmptyContextTest<MinidumpContextMIPS64Writer,
                   MinidumpContextMIPS64,
                   TypeParam>(ExpectMinidumpContextMIPS64);
}

TYPED_TEST(MinidumpContextWriter, MIPS_FromSnapshot) {
  constexpr uint32_t kSeed = 32;
  CPUContextMIPS context_mips;
  CPUContext context;
  context.mipsel = &context_mips;
  InitializeCPUContextMIPS(&context, kSeed);
  FromSnapshotTest<MinidumpContextMIPSWriter, MinidumpContextMIPS, TypeParam>(
      context, ExpectMinidumpContextMIPS, kSeed);
}

TYPED_TEST(MinidumpContextWriter, MIPS64_FromSnapshot) {
  constexpr uint32_t kSeed = 64;
  CPUContextMIPS64 context_mips;
  CPUContext context;
  context.mips64 = &context_mips;
  InitializeCPUContextMIPS64(&context, kSeed);
  FromSnapshotTest<MinidumpContextMIPS64Writer,
                   MinidumpContextMIPS64,
                   TypeParam>(context, ExpectMinidumpContextMIPS64, kSeed);
}

}  // namespace
}  // namespace test
}  // namespace crashpad
