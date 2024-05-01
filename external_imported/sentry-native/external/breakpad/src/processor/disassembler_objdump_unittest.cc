// Copyright (c) 2022, Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <unistd.h>
#include <vector>

#include "breakpad_googletest_includes.h"

#include "google_breakpad/common/breakpad_types.h"
#include "google_breakpad/common/minidump_cpu_amd64.h"
#include "google_breakpad/common/minidump_cpu_x86.h"
#include "google_breakpad/processor/dump_context.h"
#include "google_breakpad/processor/memory_region.h"
#include "processor/disassembler_objdump.h"

namespace google_breakpad {
class DisassemblerObjdumpForTest : public DisassemblerObjdump {
 public:
  using DisassemblerObjdump::CalculateAddress;
  using DisassemblerObjdump::DisassembleInstruction;
  using DisassemblerObjdump::TokenizeInstruction;
};

class TestMemoryRegion : public MemoryRegion {
 public:
  TestMemoryRegion(uint64_t base, std::vector<uint8_t> bytes);
  ~TestMemoryRegion() override = default;

  uint64_t GetBase() const override;
  uint32_t GetSize() const override;

  bool GetMemoryAtAddress(uint64_t address, uint8_t* value) const override;
  bool GetMemoryAtAddress(uint64_t address, uint16_t* value) const override;
  bool GetMemoryAtAddress(uint64_t address, uint32_t* value) const override;
  bool GetMemoryAtAddress(uint64_t address, uint64_t* value) const override;

  void Print() const override;

 private:
  uint64_t base_;
  std::vector<uint8_t> bytes_;
};

TestMemoryRegion::TestMemoryRegion(uint64_t address, std::vector<uint8_t> bytes)
    : base_(address), bytes_(bytes) {}

uint64_t TestMemoryRegion::GetBase() const {
  return base_;
}

uint32_t TestMemoryRegion::GetSize() const {
  return static_cast<uint32_t>(bytes_.size());
}

bool TestMemoryRegion::GetMemoryAtAddress(uint64_t address,
                                          uint8_t* value) const {
  if (address < GetBase() ||
      address + sizeof(uint8_t) > GetBase() + GetSize()) {
    return false;
  }

  memcpy(value, &bytes_[address - GetBase()], sizeof(uint8_t));
  return true;
}

// We don't use the following functions, so no need to implement.
bool TestMemoryRegion::GetMemoryAtAddress(uint64_t address,
                                          uint16_t* value) const {
  return false;
}

bool TestMemoryRegion::GetMemoryAtAddress(uint64_t address,
                                          uint32_t* value) const {
  return false;
}

bool TestMemoryRegion::GetMemoryAtAddress(uint64_t address,
                                          uint64_t* value) const {
  return false;
}

void TestMemoryRegion::Print() const {}

const uint32_t kX86TestDs = 0x01000000;
const uint32_t kX86TestEs = 0x02000000;
const uint32_t kX86TestFs = 0x03000000;
const uint32_t kX86TestGs = 0x04000000;
const uint32_t kX86TestEax = 0x00010101;
const uint32_t kX86TestEbx = 0x00020202;
const uint32_t kX86TestEcx = 0x00030303;
const uint32_t kX86TestEdx = 0x00040404;
const uint32_t kX86TestEsi = 0x00050505;
const uint32_t kX86TestEdi = 0x00060606;
const uint32_t kX86TestEsp = 0x00070707;
const uint32_t kX86TestEbp = 0x00080808;
const uint32_t kX86TestEip = 0x23230000;

const uint64_t kAMD64TestRax = 0x0000010101010101ul;
const uint64_t kAMD64TestRbx = 0x0000020202020202ul;
const uint64_t kAMD64TestRcx = 0x0000030303030303ul;
const uint64_t kAMD64TestRdx = 0x0000040404040404ul;
const uint64_t kAMD64TestRsi = 0x0000050505050505ul;
const uint64_t kAMD64TestRdi = 0x0000060606060606ul;
const uint64_t kAMD64TestRsp = 0x0000070707070707ul;
const uint64_t kAMD64TestRbp = 0x0000080808080808ul;
const uint64_t kAMD64TestR8 = 0x0000090909090909ul;
const uint64_t kAMD64TestR9 = 0x00000a0a0a0a0a0aul;
const uint64_t kAMD64TestR10 = 0x00000b0b0b0b0b0bul;
const uint64_t kAMD64TestR11 = 0x00000c0c0c0c0c0cul;
const uint64_t kAMD64TestR12 = 0x00000d0d0d0d0d0dul;
const uint64_t kAMD64TestR13 = 0x00000e0e0e0e0e0eul;
const uint64_t kAMD64TestR14 = 0x00000f0f0f0f0f0ful;
const uint64_t kAMD64TestR15 = 0x0000001010101010ul;
const uint64_t kAMD64TestRip = 0x0000000023230000ul;

class TestDumpContext : public DumpContext {
 public:
  TestDumpContext(bool x86_64 = false);
  ~TestDumpContext() override;
};

TestDumpContext::TestDumpContext(bool x86_64) {
  if (!x86_64) {
    MDRawContextX86* raw_context = new MDRawContextX86();
    memset(raw_context, 0, sizeof(*raw_context));

    raw_context->context_flags = MD_CONTEXT_X86_FULL;

    raw_context->ds = kX86TestDs;
    raw_context->es = kX86TestEs;
    raw_context->fs = kX86TestFs;
    raw_context->gs = kX86TestGs;
    raw_context->eax = kX86TestEax;
    raw_context->ebx = kX86TestEbx;
    raw_context->ecx = kX86TestEcx;
    raw_context->edx = kX86TestEdx;
    raw_context->esi = kX86TestEsi;
    raw_context->edi = kX86TestEdi;
    raw_context->esp = kX86TestEsp;
    raw_context->ebp = kX86TestEbp;
    raw_context->eip = kX86TestEip;

    SetContextFlags(raw_context->context_flags);
    SetContextX86(raw_context);
    this->valid_ = true;
  } else {
    MDRawContextAMD64* raw_context = new MDRawContextAMD64();
    memset(raw_context, 0, sizeof(*raw_context));

    raw_context->context_flags = MD_CONTEXT_AMD64_FULL;

    raw_context->rax = kAMD64TestRax;
    raw_context->rbx = kAMD64TestRbx;
    raw_context->rcx = kAMD64TestRcx;
    raw_context->rdx = kAMD64TestRdx;
    raw_context->rsi = kAMD64TestRsi;
    raw_context->rdi = kAMD64TestRdi;
    raw_context->rsp = kAMD64TestRsp;
    raw_context->rbp = kAMD64TestRbp;
    raw_context->r8 = kAMD64TestR8;
    raw_context->r9 = kAMD64TestR9;
    raw_context->r10 = kAMD64TestR10;
    raw_context->r11 = kAMD64TestR11;
    raw_context->r12 = kAMD64TestR12;
    raw_context->r13 = kAMD64TestR13;
    raw_context->r14 = kAMD64TestR14;
    raw_context->r15 = kAMD64TestR15;
    raw_context->rip = kAMD64TestRip;

    SetContextFlags(raw_context->context_flags);
    SetContextAMD64(raw_context);
    this->valid_ = true;
  }
}

TestDumpContext::~TestDumpContext() {
  FreeContext();
}

TEST(DisassemblerObjdumpTest, DisassembleInstructionX86) {
  string instruction;
  ASSERT_FALSE(DisassemblerObjdumpForTest::DisassembleInstruction(
      MD_CONTEXT_X86, nullptr, 0, instruction));
  std::vector<uint8_t> pop_eax = {0x58};
  ASSERT_TRUE(DisassemblerObjdumpForTest::DisassembleInstruction(
      MD_CONTEXT_X86, pop_eax.data(), pop_eax.size(), instruction));
  ASSERT_EQ(instruction, "pop    eax");
}

TEST(DisassemblerObjdumpTest, DisassembleInstructionAMD64) {
  string instruction;
  ASSERT_FALSE(DisassemblerObjdumpForTest::DisassembleInstruction(
      MD_CONTEXT_AMD64, nullptr, 0, instruction));
  std::vector<uint8_t> pop_rax = {0x58};
  ASSERT_TRUE(DisassemblerObjdumpForTest::DisassembleInstruction(
      MD_CONTEXT_AMD64, pop_rax.data(), pop_rax.size(), instruction));
  ASSERT_EQ(instruction, "pop    rax");
}

TEST(DisassemblerObjdumpTest, TokenizeInstruction) {
  string operation, dest, src;
  ASSERT_TRUE(DisassemblerObjdumpForTest::TokenizeInstruction(
      "pop eax", operation, dest, src));
  ASSERT_EQ(operation, "pop");
  ASSERT_EQ(dest, "eax");

  ASSERT_TRUE(DisassemblerObjdumpForTest::TokenizeInstruction(
      "mov eax, ebx", operation, dest, src));
  ASSERT_EQ(operation, "mov");
  ASSERT_EQ(dest, "eax");
  ASSERT_EQ(src, "ebx");

  ASSERT_TRUE(DisassemblerObjdumpForTest::TokenizeInstruction(
      "pop rax", operation, dest, src));
  ASSERT_EQ(operation, "pop");
  ASSERT_EQ(dest, "rax");

  ASSERT_TRUE(DisassemblerObjdumpForTest::TokenizeInstruction(
      "mov rax, rbx", operation, dest, src));
  ASSERT_EQ(operation, "mov");
  ASSERT_EQ(dest, "rax");
  ASSERT_EQ(src, "rbx");

  // Test the three parsing failure paths
  ASSERT_FALSE(DisassemblerObjdumpForTest::TokenizeInstruction(
      "mov rax,", operation, dest, src));
  ASSERT_FALSE(DisassemblerObjdumpForTest::TokenizeInstruction(
      "mov rax rbx", operation, dest, src));
  ASSERT_FALSE(DisassemblerObjdumpForTest::TokenizeInstruction(
      "mov rax, rbx, rcx", operation, dest, src));

  // This is of course a nonsense instruction, but test that we do remove
  // multiple instruction prefixes and can handle multiple memory operands.
  ASSERT_TRUE(DisassemblerObjdumpForTest::TokenizeInstruction(
      "rep lock mov DWORD PTR rax, QWORD PTR rbx", operation, dest, src));
  ASSERT_EQ(operation, "mov");
  ASSERT_EQ(dest, "rax");
  ASSERT_EQ(src, "rbx");

  // Test that we ignore junk following a valid instruction
  ASSERT_TRUE(DisassemblerObjdumpForTest::TokenizeInstruction(
      "mov rax, rbx ; junk here", operation, dest, src));
  ASSERT_EQ(operation, "mov");
  ASSERT_EQ(dest, "rax");
  ASSERT_EQ(src, "rbx");
}

namespace x86 {
const TestMemoryRegion load_reg(kX86TestEip, {0x8b, 0x06});  // mov eax, [esi];

const TestMemoryRegion load_reg_index(kX86TestEip,
                                      {0x8b, 0x04,
                                       0xbe});  // mov eax, [esi+edi*4];

const TestMemoryRegion load_reg_offset(kX86TestEip,
                                       {0x8b, 0x46,
                                        0x10});  // mov eax, [esi+0x10];

const TestMemoryRegion load_reg_index_offset(
    kX86TestEip,
    {0x8b, 0x44, 0xbe, 0xf0});  // mov eax, [esi+edi*4-0x10];

const TestMemoryRegion rep_stosb(kX86TestEip, {0xf3, 0xaa});  // rep stosb;

const TestMemoryRegion lock_cmpxchg(kX86TestEip,
                                    {0xf0, 0x0f, 0xb1, 0x46,
                                     0x10});  // lock cmpxchg [esi + 0x10], eax;

const TestMemoryRegion call_reg_offset(kX86TestEip,
                                       {0xff, 0x96, 0x99, 0x99, 0x99,
                                        0x09});  // call [esi+0x9999999];
}  // namespace x86

TEST(DisassemblerObjdumpTest, X86LoadReg) {
  TestDumpContext context;
  DisassemblerObjdump dis(context.GetContextCPU(), &x86::load_reg, kX86TestEip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_FALSE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_TRUE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(src_address, kX86TestEsi);
}

TEST(DisassemblerObjdumpTest, X86LoadRegIndex) {
  TestDumpContext context;
  DisassemblerObjdump dis(context.GetContextCPU(), &x86::load_reg_index,
                          kX86TestEip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_FALSE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_TRUE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(src_address, kX86TestEsi + (kX86TestEdi * 4));
}

TEST(DisassemblerObjdumpTest, X86LoadRegOffset) {
  TestDumpContext context;
  DisassemblerObjdump dis(context.GetContextCPU(), &x86::load_reg_offset,
                          kX86TestEip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_FALSE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_TRUE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(src_address, kX86TestEsi + 0x10);
}

TEST(DisassemblerObjdumpTest, X86LoadRegIndexOffset) {
  TestDumpContext context;
  DisassemblerObjdump dis(context.GetContextCPU(), &x86::load_reg_index_offset,
                          kX86TestEip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_FALSE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_TRUE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(src_address, kX86TestEsi + (kX86TestEdi * 4) - 0x10);
}

TEST(DisassemblerObjdumpTest, X86RepStosb) {
  TestDumpContext context;
  DisassemblerObjdump dis(context.GetContextCPU(), &x86::rep_stosb,
                          kX86TestEip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_TRUE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_FALSE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(dest_address, kX86TestEs + kX86TestEdi);
}

TEST(DisassemblerObjdumpTest, X86LockCmpxchg) {
  TestDumpContext context;
  DisassemblerObjdump dis(context.GetContextCPU(), &x86::lock_cmpxchg,
                          kX86TestEip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_TRUE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_FALSE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(dest_address, kX86TestEsi + 0x10);
}

TEST(DisassemblerObjdumpTest, X86CallRegOffset) {
  TestDumpContext context;
  DisassemblerObjdump dis(context.GetContextCPU(), &x86::call_reg_offset,
                          kX86TestEip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_TRUE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_FALSE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(dest_address, kX86TestEsi + 0x9999999);
}

namespace amd64 {
const TestMemoryRegion load_reg(kAMD64TestRip,
                                {0x48, 0x8b, 0x06});  // mov rax, [rsi];

const TestMemoryRegion load_reg_index(kAMD64TestRip,
                                      {0x48, 0x8b, 0x04,
                                       0xbe});  // mov rax, [rsi+rdi*4];

const TestMemoryRegion load_rip_relative(kAMD64TestRip,
                                         {0x48, 0x8b, 0x05, 0x10, 0x00, 0x00,
                                          0x00});  // mov rax, [rip+0x10];

const TestMemoryRegion load_reg_index_offset(
    kAMD64TestRip,
    {0x48, 0x8b, 0x44, 0xbe, 0xf0});  // mov rax, [rsi+rdi*4-0x10];

const TestMemoryRegion rep_stosb(kAMD64TestRip, {0xf3, 0xaa});  // rep stosb;

const TestMemoryRegion lock_cmpxchg(kAMD64TestRip,
                                    {0xf0, 0x48, 0x0f, 0xb1, 0x46,
                                     0x10});  // lock cmpxchg [rsi + 0x10], rax;

const TestMemoryRegion call_reg_offset(kAMD64TestRip,
                                       {0xff, 0x96, 0x99, 0x99, 0x99,
                                        0x09});  // call [rsi+0x9999999];
}  // namespace amd64

TEST(DisassemblerObjdumpTest, AMD64LoadReg) {
  TestDumpContext context(true);
  DisassemblerObjdump dis(context.GetContextCPU(), &amd64::load_reg,
                          kAMD64TestRip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_FALSE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_TRUE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(src_address, kAMD64TestRsi);
}

TEST(DisassemblerObjdumpTest, AMD64LoadRegIndex) {
  TestDumpContext context(true);
  DisassemblerObjdump dis(context.GetContextCPU(), &amd64::load_reg_index,
                          kAMD64TestRip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_FALSE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_TRUE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(src_address, kAMD64TestRsi + (kAMD64TestRdi * 4));
}

TEST(DisassemblerObjdumpTest, AMD64LoadRipRelative) {
  TestDumpContext context(true);
  DisassemblerObjdump dis(context.GetContextCPU(), &amd64::load_rip_relative,
                          kAMD64TestRip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_FALSE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_TRUE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(src_address, kAMD64TestRip + 0x10);
}

TEST(DisassemblerObjdumpTest, AMD64LoadRegIndexOffset) {
  TestDumpContext context(true);
  DisassemblerObjdump dis(context.GetContextCPU(),
                          &amd64::load_reg_index_offset, kAMD64TestRip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_FALSE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_TRUE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(src_address, kAMD64TestRsi + (kAMD64TestRdi * 4) - 0x10);
}

TEST(DisassemblerObjdumpTest, AMD64RepStosb) {
  TestDumpContext context(true);
  DisassemblerObjdump dis(context.GetContextCPU(), &amd64::rep_stosb,
                          kAMD64TestRip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_TRUE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_FALSE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(dest_address, kAMD64TestRdi);
}

TEST(DisassemblerObjdumpTest, AMD64LockCmpxchg) {
  TestDumpContext context(true);
  DisassemblerObjdump dis(context.GetContextCPU(), &amd64::lock_cmpxchg,
                          kAMD64TestRip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_TRUE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_FALSE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(dest_address, kAMD64TestRsi + 0x10);
}

TEST(DisassemblerObjdumpTest, AMD64CallRegOffset) {
  TestDumpContext context(true);
  DisassemblerObjdump dis(context.GetContextCPU(), &amd64::call_reg_offset,
                          kAMD64TestRip);
  uint64_t src_address = 0, dest_address = 0;
  ASSERT_TRUE(dis.CalculateDestAddress(context, dest_address));
  ASSERT_FALSE(dis.CalculateSrcAddress(context, src_address));
  ASSERT_EQ(dest_address, kAMD64TestRsi + 0x9999999);
}
}  // namespace google_breakpad
