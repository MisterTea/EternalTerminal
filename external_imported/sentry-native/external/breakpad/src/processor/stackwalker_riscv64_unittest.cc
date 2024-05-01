// Copyright 2013 Google LLC
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

/* stackwalker_riscv64_unittest.cc: Unit tests for StackwalkerRISCV64 class.
 *
 * Author: Iacopo Colonnelli
 */

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <string.h>
#include <string>
#include <vector>

#include "breakpad_googletest_includes.h"
#include "common/test_assembler.h"
#include "common/using_std_string.h"
#include "google_breakpad/common/minidump_format.h"
#include "google_breakpad/processor/basic_source_line_resolver.h"
#include "google_breakpad/processor/call_stack.h"
#include "google_breakpad/processor/code_module.h"
#include "google_breakpad/processor/source_line_resolver_interface.h"
#include "google_breakpad/processor/stack_frame_cpu.h"
#include "processor/stackwalker_unittest_utils.h"
#include "processor/stackwalker_riscv64.h"
#include "processor/windows_frame_info.h"

using google_breakpad::BasicSourceLineResolver;
using google_breakpad::CallStack;
using google_breakpad::CodeModule;
using google_breakpad::StackFrameSymbolizer;
using google_breakpad::StackFrame;
using google_breakpad::StackFrameRISCV64;
using google_breakpad::Stackwalker;
using google_breakpad::StackwalkerRISCV64;
using google_breakpad::SystemInfo;
using google_breakpad::WindowsFrameInfo;
using google_breakpad::test_assembler::kLittleEndian;
using google_breakpad::test_assembler::Label;
using google_breakpad::test_assembler::Section;
using std::vector;
using testing::_;
using testing::AnyNumber;
using testing::DoAll;
using testing::Return;
using testing::SetArgumentPointee;
using testing::Test;

class StackwalkerRISCV64Fixture {
public:
  StackwalkerRISCV64Fixture()
      : stack_section(kLittleEndian),
      // Give the two modules reasonable standard locations and names
      // for tests to play with.
        module1(0x40000000, 0x10000, "module1", "version1"),
        module2(0x50000000, 0x10000, "module2", "version2") {
    // Identify the system as an iOS system.
    system_info.os = "iOS";
    system_info.os_short = "ios";
    system_info.cpu = "riscv64";
    system_info.cpu_info = "";

    // Put distinctive values in the raw CPU context.
    BrandContext(&raw_context);

    // Create some modules with some stock debugging information.
    modules.Add(&module1);
    modules.Add(&module2);

    // By default, none of the modules have symbol info; call
    // SetModuleSymbols to override this.
    EXPECT_CALL(supplier, GetCStringSymbolData(_, _, _, _, _))
        .WillRepeatedly(Return(MockSymbolSupplier::NOT_FOUND));

    // Avoid GMOCK WARNING "Uninteresting mock function call - returning
    // directly" for FreeSymbolData().
    EXPECT_CALL(supplier, FreeSymbolData(_)).Times(AnyNumber());

    // Reset max_frames_scanned since it's static.
    Stackwalker::set_max_frames_scanned(1024);
  }

  // Set the Breakpad symbol information that supplier should return for
  // MODULE to INFO.
  void SetModuleSymbols(MockCodeModule* module, const string& info) {
    size_t buffer_size;
    char *buffer = supplier.CopySymbolDataAndOwnTheCopy(info, &buffer_size);
    EXPECT_CALL(supplier, GetCStringSymbolData(module, &system_info, _, _, _))
        .WillRepeatedly(DoAll(SetArgumentPointee<3>(buffer),
                              SetArgumentPointee<4>(buffer_size),
                              Return(MockSymbolSupplier::FOUND)));
  }

  // Populate stack_region with the contents of stack_section. Use
  // stack_section.start() as the region's starting address.
  void RegionFromSection() {
    string contents;
    ASSERT_TRUE(stack_section.GetContents(&contents));
    stack_region.Init(stack_section.start().Value(), contents);
  }

  // Fill RAW_CONTEXT with pseudo-random data, for round-trip checking.
  void BrandContext(MDRawContextRISCV64 *raw_context) {
    uint8_t x = 173;
    for (size_t i = 0; i < sizeof(*raw_context); i++)
      reinterpret_cast<uint8_t*>(raw_context)[i] = (x += 17);
  }

  SystemInfo system_info;
  MDRawContextRISCV64 raw_context;
  Section stack_section;
  MockMemoryRegion stack_region;
  MockCodeModule module1;
  MockCodeModule module2;
  MockCodeModules modules;
  MockSymbolSupplier supplier;
  BasicSourceLineResolver resolver;
  CallStack call_stack;
  const vector<StackFrame*>* frames;
};

class SanityCheck: public StackwalkerRISCV64Fixture, public Test { };

TEST_F(SanityCheck, NoResolver) {
  // Since the context's frame pointer is garbage, the stack walk will end after
  // the first frame.
  StackFrameSymbolizer frame_symbolizer(NULL, NULL);
  StackwalkerRISCV64 walker(&system_info, &raw_context, &stack_region, &modules,
                            &frame_symbolizer);
  // This should succeed even without a resolver or supplier.
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(0U, modules_without_symbols.size());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(1U, frames->size());
  StackFrameRISCV64 *frame = static_cast<StackFrameRISCV64*>(frames->at(0));
  // Check that the values from the original raw context made it
  // through to the context in the stack frame.
  EXPECT_EQ(0, memcmp(&raw_context, &frame->context, sizeof(raw_context)));
}

class GetContextFrame: public StackwalkerRISCV64Fixture, public Test { };

// The stackwalker should be able to produce the context frame even
// without stack memory present.
TEST_F(GetContextFrame, NoStackMemory) {
  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV64 walker(&system_info, &raw_context, NULL, &modules,
                            &frame_symbolizer);
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(0U, modules_without_symbols.size());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(1U, frames->size());
  StackFrameRISCV64 *frame = static_cast<StackFrameRISCV64*>(frames->at(0));
  // Check that the values from the original raw context made it
  // through to the context in the stack frame.
  EXPECT_EQ(0, memcmp(&raw_context, &frame->context, sizeof(raw_context)));
}

class GetCallerFrame: public StackwalkerRISCV64Fixture, public Test { };

TEST_F(GetCallerFrame, ScanWithoutSymbols) {
  // When the stack walker resorts to scanning the stack,
  // only addresses located within loaded modules are
  // considered valid return addresses.
  // Force scanning through three frames to ensure that the
  // stack pointer is set properly in scan-recovered frames.
  stack_section.start() = 0x80000000;
  uint64_t return_address1 = 0x50000100;
  uint64_t return_address2 = 0x50000900;
  Label frame1_sp, frame2_sp;
  stack_section
      // frame 0
      .Append(16, 0)                      // space

      .D64(0x40090000)                    // junk that's not
      .D64(0x60000000)                    // a return address

      .D64(return_address1)               // actual return address
      // frame 1
      .Mark(&frame1_sp)
      .Append(16, 0)                      // space

      .D64(0xF0000000)                    // more junk
      .D64(0x0000000D)

      .D64(return_address2)               // actual return address
      // frame 2
      .Mark(&frame2_sp)
      .Append(64, 0);                     // end of stack
  RegionFromSection();

  raw_context.pc = 0x40005510;
  raw_context.sp = stack_section.start().Value();

  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV64 walker(&system_info, &raw_context, &stack_region, &modules,
                            &frame_symbolizer);
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(2U, modules_without_symbols.size());
  ASSERT_EQ("module1", modules_without_symbols[0]->debug_file());
  ASSERT_EQ("module2", modules_without_symbols[1]->debug_file());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(3U, frames->size());

  StackFrameRISCV64 *frame0 = static_cast<StackFrameRISCV64*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV64::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));

  StackFrameRISCV64 *frame1 = static_cast<StackFrameRISCV64*>(frames->at(1));
  EXPECT_EQ(StackFrame::FRAME_TRUST_SCAN, frame1->trust);
  ASSERT_EQ((StackFrameRISCV64::CONTEXT_VALID_PC |
             StackFrameRISCV64::CONTEXT_VALID_SP),
             frame1->context_validity);
  EXPECT_EQ(return_address1, frame1->context.pc);
  EXPECT_EQ(frame1_sp.Value(), frame1->context.sp);

  StackFrameRISCV64 *frame2 = static_cast<StackFrameRISCV64*>(frames->at(2));
  EXPECT_EQ(StackFrame::FRAME_TRUST_SCAN, frame2->trust);
  ASSERT_EQ((StackFrameRISCV64::CONTEXT_VALID_PC |
             StackFrameRISCV64::CONTEXT_VALID_SP),
             frame2->context_validity);
  EXPECT_EQ(return_address2, frame2->context.pc);
  EXPECT_EQ(frame2_sp.Value(), frame2->context.sp);
}

TEST_F(GetCallerFrame, ScanWithFunctionSymbols) {
  // During stack scanning, if a potential return address
  // is located within a loaded module that has symbols,
  // it is only considered a valid return address if it
  // lies within a function's bounds.
  stack_section.start() = 0x80000000;
  uint64_t return_address = 0x50000200;
  Label frame1_sp;

  stack_section
      // frame 0
      .Append(16, 0)                      // space

      .D64(0x40090000)                    // junk that's not
      .D64(0x60000000)                    // a return address

      .D64(0x40001000)                    // a couple of plausible addresses
      .D64(0x5000F000)                    // that are not within functions

      .D64(return_address)                // actual return address
      // frame 1
      .Mark(&frame1_sp)
      .Append(64, 0);                     // end of stack
  RegionFromSection();

  raw_context.pc = 0x40000200;
  raw_context.sp = stack_section.start().Value();

  SetModuleSymbols(&module1,
      // The youngest frame's function.
      "FUNC 100 400 10 monotreme\n");
  SetModuleSymbols(&module2,
      // The calling frame's function.
      "FUNC 100 400 10 marsupial\n");

  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV64 walker(&system_info, &raw_context, &stack_region,
                            &modules, &frame_symbolizer);
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(0U, modules_without_symbols.size());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(2U, frames->size());

  StackFrameRISCV64 *frame0 = static_cast<StackFrameRISCV64*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV64::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));
  EXPECT_EQ("monotreme", frame0->function_name);
  EXPECT_EQ(0x40000100ULL, frame0->function_base);

  StackFrameRISCV64 *frame1 = static_cast<StackFrameRISCV64*>(frames->at(1));
  EXPECT_EQ(StackFrame::FRAME_TRUST_SCAN, frame1->trust);
  ASSERT_EQ((StackFrameRISCV64::CONTEXT_VALID_PC |
             StackFrameRISCV64::CONTEXT_VALID_SP),
             frame1->context_validity);
  EXPECT_EQ(return_address, frame1->context.pc);
  EXPECT_EQ(frame1_sp.Value(), frame1->context.sp);
  EXPECT_EQ("marsupial", frame1->function_name);
  EXPECT_EQ(0x50000100ULL, frame1->function_base);
}

TEST_F(GetCallerFrame, ScanFirstFrame) {
  // If the stackwalker resorts to stack scanning, it will scan much
  // farther to find the caller of the context frame.
  stack_section.start() = 0x80000000;
  uint64_t return_address1 = 0x50000100;
  uint64_t return_address2 = 0x50000900;
  Label frame1_sp, frame2_sp;
  stack_section
      // frame 0
      .Append(32, 0)                      // space

      .D64(0x40090000)                    // junk that's not
      .D64(0x60000000)                    // a return address

      .Append(96, 0)                      // more space

      .D64(return_address1)               // actual return address
      // frame 1
      .Mark(&frame1_sp)
      .Append(32, 0)                      // space

      .D64(0xF0000000)                    // more junk
      .D64(0x0000000D)

      .Append(336, 0)                     // more space

      .D64(return_address2)               // actual return address
      // (won't be found)
      // frame 2
      .Mark(&frame2_sp)
      .Append(64, 0);                     // end of stack
  RegionFromSection();

  raw_context.pc = 0x40005510;
  raw_context.sp = stack_section.start().Value();

  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV64 walker(&system_info, &raw_context, &stack_region,
                            &modules, &frame_symbolizer);
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(2U, modules_without_symbols.size());
  ASSERT_EQ("module1", modules_without_symbols[0]->debug_file());
  ASSERT_EQ("module2", modules_without_symbols[1]->debug_file());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(2U, frames->size());

  StackFrameRISCV64 *frame0 = static_cast<StackFrameRISCV64*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV64::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));

  StackFrameRISCV64 *frame1 = static_cast<StackFrameRISCV64*>(frames->at(1));
  EXPECT_EQ(StackFrame::FRAME_TRUST_SCAN, frame1->trust);
  ASSERT_EQ((StackFrameRISCV64::CONTEXT_VALID_PC |
             StackFrameRISCV64::CONTEXT_VALID_SP),
             frame1->context_validity);
  EXPECT_EQ(return_address1, frame1->context.pc);
  EXPECT_EQ(frame1_sp.Value(), frame1->context.sp);
}

// Test that set_max_frames_scanned prevents using stack scanning
// to find caller frames.
TEST_F(GetCallerFrame, ScanningNotAllowed) {
  // When the stack walker resorts to scanning the stack,
  // only addresses located within loaded modules are
  // considered valid return addresses.
  stack_section.start() = 0x80000000;
  uint64_t return_address1 = 0x50000100;
  uint64_t return_address2 = 0x50000900;
  Label frame1_sp, frame2_sp;
  stack_section
      // frame 0
      .Append(16, 0)                      // space

      .D64(0x40090000)                    // junk that's not
      .D64(0x60000000)                    // a return address

      .D64(return_address1)               // actual return address
      // frame 1
      .Mark(&frame1_sp)
      .Append(16, 0)                      // space

      .D64(0xF0000000)                    // more junk
      .D64(0x0000000D)

      .D64(return_address2)               // actual return address
      // frame 2
      .Mark(&frame2_sp)
      .Append(64, 0);                     // end of stack
  RegionFromSection();

  raw_context.pc = 0x40005510;
  raw_context.sp = stack_section.start().Value();

  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV64 walker(&system_info, &raw_context, &stack_region,
                            &modules, &frame_symbolizer);
  Stackwalker::set_max_frames_scanned(0);

  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(1U, modules_without_symbols.size());
  ASSERT_EQ("module1", modules_without_symbols[0]->debug_file());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(1U, frames->size());

  StackFrameRISCV64 *frame0 = static_cast<StackFrameRISCV64*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV64::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));
}

class GetFramesByFramePointer:
    public StackwalkerRISCV64Fixture,
    public Test { };

TEST_F(GetFramesByFramePointer, OnlyFramePointer) {
  stack_section.start() = 0x80000000;
  uint64_t return_address1 = 0x50000100;
  uint64_t return_address2 = 0x50000900;
  Label frame1_sp, frame2_sp;
  Label frame1_fp, frame2_fp;
  stack_section
      // frame 0
      .Append(64, 0)           // Whatever values on the stack.
      .D64(0x0000000D)         // junk that's not
      .D64(0xF0000000)         // a return address.

      .Mark(&frame1_fp)        // Next fp will point to the next value.
      .D64(frame2_fp)          // Save current frame pointer.
      .D64(return_address2)    // Save current link register.
      .Mark(&frame1_sp)

      // frame 1
      .Append(64, 0)           // Whatever values on the stack.
      .D64(0x0000000D)         // junk that's not
      .D64(0xF0000000)         // a return address.

      .Mark(&frame2_fp)
      .D64(0)
      .D64(0)
      .Mark(&frame2_sp)

      // frame 2
      .Append(64, 0)           // Whatever values on the stack.
      .D64(0x0000000D)         // junk that's not
      .D64(0xF0000000);        // a return address.
  RegionFromSection();


  raw_context.pc = 0x40005510;
  raw_context.ra = return_address1;
  raw_context.s0 = frame1_fp.Value();
  raw_context.sp = stack_section.start().Value();

  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV64 walker(&system_info, &raw_context,
                            &stack_region, &modules, &frame_symbolizer);

  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(2U, modules_without_symbols.size());
  ASSERT_EQ("module1", modules_without_symbols[0]->debug_file());
  ASSERT_EQ("module2", modules_without_symbols[1]->debug_file());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(3U, frames->size());

  StackFrameRISCV64 *frame0 = static_cast<StackFrameRISCV64*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV64::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));

  StackFrameRISCV64 *frame1 = static_cast<StackFrameRISCV64*>(frames->at(1));
  EXPECT_EQ(StackFrame::FRAME_TRUST_FP, frame1->trust);
  ASSERT_EQ((StackFrameRISCV64::CONTEXT_VALID_PC |
             StackFrameRISCV64::CONTEXT_VALID_RA |
             StackFrameRISCV64::CONTEXT_VALID_S0 |
             StackFrameRISCV64::CONTEXT_VALID_SP),
             frame1->context_validity);
  EXPECT_EQ(return_address1, frame1->context.pc);
  EXPECT_EQ(return_address2, frame1->context.ra);
  EXPECT_EQ(frame1_sp.Value(), frame1->context.sp);
  EXPECT_EQ(frame2_fp.Value(), frame1->context.s0);

  StackFrameRISCV64 *frame2 = static_cast<StackFrameRISCV64*>(frames->at(2));
  EXPECT_EQ(StackFrame::FRAME_TRUST_FP, frame2->trust);
  ASSERT_EQ((StackFrameRISCV64::CONTEXT_VALID_PC |
             StackFrameRISCV64::CONTEXT_VALID_RA |
             StackFrameRISCV64::CONTEXT_VALID_S0 |
             StackFrameRISCV64::CONTEXT_VALID_SP),
             frame2->context_validity);
  EXPECT_EQ(return_address2, frame2->context.pc);
  EXPECT_EQ(0U, frame2->context.ra);
  EXPECT_EQ(frame2_sp.Value(), frame2->context.sp);
  EXPECT_EQ(0U, frame2->context.s0);
}

struct CFIFixture: public StackwalkerRISCV64Fixture {
  CFIFixture() {
    // Provide a bunch of STACK CFI records; we'll walk to the caller
    // from every point in this series, expecting to find the same set
    // of register values.
    SetModuleSymbols(&module1,
        // The youngest frame's function.
                     "FUNC 4000 1000 10 enchiridion\n"
                     // Initially, nothing has been pushed on the stack,
                     // and the return address is still in the return
                     // address register (ra).
                     "STACK CFI INIT 4000 100 .cfa: sp 0 + .ra: ra\n"
                     // Push s1, s2, the frame pointer (s0) and the
                     // return address register.
                     "STACK CFI 4001 .cfa: sp 32 + .ra: .cfa -8 + ^"
                     " s1: .cfa -32 + ^ s2: .cfa -24 + ^ "
                     " s0: .cfa -16 + ^\n"
                     // Save s1..s4 in a1..a4: verify that we populate
                     // the youngest frame with all the values we have.
                     "STACK CFI 4002 s1: a1 s2: a2 s3: a3 s4: a4\n"
                     // Restore s1..s4. Save the non-callee-saves register a2.
                     "STACK CFI 4003 .cfa: sp 40 + a2: .cfa 40 - ^"
                     " s1: s1 s2: s2 s3: s3 s4: s4\n"
                     // Move the .cfa back eight bytes, to point at the return
                     // address, and restore the sp explicitly.
                     "STACK CFI 4005 .cfa: sp 32 + a2: .cfa 32 - ^"
                     " s0: .cfa 8 - ^ .ra: .cfa ^ sp: .cfa 8 +\n"
                     // Recover the PC explicitly from a new stack slot;
                     // provide garbage for the .ra.
                     "STACK CFI 4006 .cfa: sp 40 + pc: .cfa 40 - ^\n"

                     // The calling function.
                     "FUNC 5000 1000 10 epictetus\n"
                     // Mark it as end of stack.
                     "STACK CFI INIT 5000 1000 .cfa: 0 .ra: 0\n"

                     // A function whose CFI makes the stack pointer
                     // go backwards.
                     "FUNC 6000 1000 20 palinal\n"
                     "STACK CFI INIT 6000 1000 .cfa: sp 8 - .ra: ra\n"

                     // A function with CFI expressions that can't be
                     // evaluated.
                     "FUNC 7000 1000 20 rhetorical\n"
                     "STACK CFI INIT 7000 1000 .cfa: moot .ra: ambiguous\n");

    // Provide some distinctive values for the caller's registers.
    expected.pc  = 0x0000000040005510L;
    expected.sp  = 0x0000000080000000L;
    expected.s1  = 0x5e68b5d5b5d55e68L;
    expected.s2  = 0x34f3ebd1ebd134f3L;
    expected.s3  = 0x74bca31ea31e74bcL;
    expected.s4  = 0x16b32dcb2dcb16b3L;
    expected.s5  = 0x21372ada2ada2137L;
    expected.s6  = 0x557dbbbbbbbb557dL;
    expected.s7  = 0x8ca748bf48bf8ca7L;
    expected.s8  = 0x21f0ab46ab4621f0L;
    expected.s9  = 0x146732b732b71467L;
    expected.s10 = 0xa673645fa673645fL;
    expected.s11 = 0xa673645fa673645fL;
    expected.s0  = 0xe11081128112e110L;

    // Expect CFI to recover all callee-saves registers. Since CFI is the
    // only stack frame construction technique we have, aside from the
    // context frame itself, there's no way for us to have a set of valid
    // registers smaller than this.
    expected_validity = (StackFrameRISCV64::CONTEXT_VALID_PC  |
                         StackFrameRISCV64::CONTEXT_VALID_SP  |
                         StackFrameRISCV64::CONTEXT_VALID_S1  |
                         StackFrameRISCV64::CONTEXT_VALID_S2  |
                         StackFrameRISCV64::CONTEXT_VALID_S3  |
                         StackFrameRISCV64::CONTEXT_VALID_S4  |
                         StackFrameRISCV64::CONTEXT_VALID_S5  |
                         StackFrameRISCV64::CONTEXT_VALID_S6  |
                         StackFrameRISCV64::CONTEXT_VALID_S7  |
                         StackFrameRISCV64::CONTEXT_VALID_S8  |
                         StackFrameRISCV64::CONTEXT_VALID_S9  |
                         StackFrameRISCV64::CONTEXT_VALID_S10 |
                         StackFrameRISCV64::CONTEXT_VALID_S11 |
                         StackFrameRISCV64::CONTEXT_VALID_S0);

    // By default, context frames provide all registers, as normal.
    context_frame_validity = StackFrameRISCV64::CONTEXT_VALID_ALL;

    // By default, registers are unchanged.
    raw_context = expected;
  }

  // Walk the stack, using stack_section as the contents of the stack
  // and raw_context as the current register values. (Set the stack
  // pointer to the stack's starting address.) Expect two stack
  // frames; in the older frame, expect the callee-saves registers to
  // have values matching those in 'expected'.
  void CheckWalk() {
    RegionFromSection();
    raw_context.sp = stack_section.start().Value();

    StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
    StackwalkerRISCV64 walker(&system_info, &raw_context, &stack_region,
                              &modules, &frame_symbolizer);
    walker.SetContextFrameValidity(context_frame_validity);
    vector<const CodeModule*> modules_without_symbols;
    vector<const CodeModule*> modules_with_corrupt_symbols;
    ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
                            &modules_with_corrupt_symbols));
    ASSERT_EQ(0U, modules_without_symbols.size());
    ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
    frames = call_stack.frames();
    ASSERT_EQ(2U, frames->size());

    StackFrameRISCV64 *frame0 = static_cast<StackFrameRISCV64*>(frames->at(0));
    EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
    ASSERT_EQ(context_frame_validity, frame0->context_validity);
    EXPECT_EQ("enchiridion", frame0->function_name);
    EXPECT_EQ(0x0000000040004000UL, frame0->function_base);

    StackFrameRISCV64 *frame1 = static_cast<StackFrameRISCV64*>(frames->at(1));
    EXPECT_EQ(StackFrame::FRAME_TRUST_CFI, frame1->trust);
    ASSERT_EQ(expected_validity, frame1->context_validity);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_A2)
      EXPECT_EQ(expected.a2, frame1->context.a2);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S1)
      EXPECT_EQ(expected.s1, frame1->context.s1);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S2)
      EXPECT_EQ(expected.s2, frame1->context.s2);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S3)
      EXPECT_EQ(expected.s3, frame1->context.s3);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S4)
      EXPECT_EQ(expected.s4, frame1->context.s4);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S5)
      EXPECT_EQ(expected.s5, frame1->context.s5);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S6)
      EXPECT_EQ(expected.s6, frame1->context.s6);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S7)
      EXPECT_EQ(expected.s7, frame1->context.s7);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S8)
      EXPECT_EQ(expected.s8, frame1->context.s8);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S9)
      EXPECT_EQ(expected.s9, frame1->context.s9);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S10)
      EXPECT_EQ(expected.s10, frame1->context.s10);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S11)
      EXPECT_EQ(expected.s11, frame1->context.s11);
    if (expected_validity & StackFrameRISCV64::CONTEXT_VALID_S0)
      EXPECT_EQ(expected.s0, frame1->context.s0);

    // We would never have gotten a frame in the first place if the SP
    // and PC weren't valid or ->instruction weren't set.
    EXPECT_EQ(expected.sp, frame1->context.sp);
    EXPECT_EQ(expected.pc, frame1->context.pc);
    EXPECT_EQ(expected.pc, frame1->instruction + 4);
    EXPECT_EQ("epictetus", frame1->function_name);
  }

  // The values we expect to find for the caller's registers.
  MDRawContextRISCV64 expected;

  // The validity mask for expected.
  int expected_validity;

  // The validity mask to impose on the context frame.
  int context_frame_validity;
};

class CFI: public CFIFixture, public Test { };

TEST_F(CFI, At4000) {
  stack_section.start() = expected.sp;
  raw_context.pc = 0x0000000040004000L;
  raw_context.ra = 0x0000000040005510L;
  CheckWalk();
}

TEST_F(CFI, At4001) {
  Label frame1_sp = expected.sp;
  stack_section
      .D64(0x5e68b5d5b5d55e68L) // saved s1
      .D64(0x34f3ebd1ebd134f3L) // saved s2
      .D64(0xe11081128112e110L) // saved s0
      .D64(0x0000000040005510L) // return address
      .Mark(&frame1_sp);        // This effectively sets stack_section.start().
  raw_context.pc = 0x0000000040004001L;
  // distinct callee s1, s2 and s0
  raw_context.s1 = 0xadc9f635a635adc9L;
  raw_context.s2 = 0x623135ac35ac6231L;
  raw_context.s0 = 0x5fc4be14be145fc4L;
  CheckWalk();
}

// As above, but unwind from a context that has only the PC and SP.
TEST_F(CFI, At4001LimitedValidity) {
  Label frame1_sp = expected.sp;
  stack_section
      .D64(0x5e68b5d5b5d55e68L) // saved s1
      .D64(0x34f3ebd1ebd134f3L) // saved s2
      .D64(0xe11081128112e110L) // saved s0
      .D64(0x0000000040005510L) // return address
      .Mark(&frame1_sp);        // This effectively sets stack_section.start().
  context_frame_validity = StackFrameRISCV64::CONTEXT_VALID_PC |
                           StackFrameRISCV64::CONTEXT_VALID_SP;
  raw_context.pc = 0x0000000040004001L;
  raw_context.s0 = 0x5fc4be14be145fc4L;

  expected_validity = (StackFrameRISCV64::CONTEXT_VALID_PC |
                       StackFrameRISCV64::CONTEXT_VALID_SP |
                       StackFrameRISCV64::CONTEXT_VALID_S0 |
                       StackFrameRISCV64::CONTEXT_VALID_S1 |
                       StackFrameRISCV64::CONTEXT_VALID_S2);
  CheckWalk();
}

TEST_F(CFI, At4002) {
  Label frame1_sp = expected.sp;
  stack_section
      .D64(0xff3dfb81fb81ff3dL) // no longer saved s1
      .D64(0x34f3ebd1ebd134f3L) // no longer saved s2
      .D64(0xe11081128112e110L) // saved s0
      .D64(0x0000000040005510L) // return address
      .Mark(&frame1_sp);        // This effectively sets stack_section.start().
  raw_context.pc = 0x0000000040004002L;
  raw_context.a1 = 0x5e68b5d5b5d55e68L;  // saved s1
  raw_context.a2 = 0x34f3ebd1ebd134f3L;  // saved s2
  raw_context.a3 = 0x74bca31ea31e74bcL;  // saved s3
  raw_context.a4 = 0x16b32dcb2dcb16b3L;  // saved s4
  raw_context.s1 = 0xadc9f635a635adc9L;  // distinct callee s1
  raw_context.s2 = 0x623135ac35ac6231L;  // distinct callee s2
  raw_context.s3 = 0xac4543564356ac45L;  // distinct callee s3
  raw_context.s4 = 0x2561562f562f2561L;  // distinct callee s4
  // distinct callee s0
  raw_context.s0 = 0x5fc4be14be145fc4L;
  CheckWalk();
}

TEST_F(CFI, At4003) {
  Label frame1_sp = expected.sp;
  stack_section
      .D64(0xdd5a48c848c8dd5aL) // saved a2 (even though it's not callee-saves)
      .D64(0xff3dfb81fb81ff3dL) // no longer saved s1
      .D64(0x34f3ebd1ebd134f3L) // no longer saved s2
      .D64(0xe11081128112e110L) // saved s0
      .D64(0x0000000040005510L) // return address
      .Mark(&frame1_sp);        // This effectively sets stack_section.start().
  raw_context.pc = 0x0000000040004003L;
  // distinct callee a2 and fp
  raw_context.a2 = 0xfb756319fb756319L;
  raw_context.s0 = 0x5fc4be14be145fc4L;
  // caller's a2
  expected.a2 = 0xdd5a48c848c8dd5aL;
  expected_validity |= StackFrameRISCV64::CONTEXT_VALID_A2;
  CheckWalk();
}

// We have no new rule at module offset 0x4004, so the results here should
// be the same as those at module offset 0x4003.
TEST_F(CFI, At4004) {
  Label frame1_sp = expected.sp;
  stack_section
      .D64(0xdd5a48c848c8dd5aL) // saved a2 (even though it's not callee-saves)
      .D64(0xff3dfb81fb81ff3dL) // no longer saved s1
      .D64(0x34f3ebd1ebd134f3L) // no longer saved s2
      .D64(0xe11081128112e110L) // saved s0
      .D64(0x0000000040005510L) // return address
      .Mark(&frame1_sp);        // This effectively sets stack_section.start().
  raw_context.pc = 0x0000000040004004L;
  // distinct callee a2 and s0
  raw_context.a2 = 0xfb756319fb756319L;
  raw_context.s0 = 0x5fc4be14be145fc4L;
  // caller's a2
  expected.a2 = 0xdd5a48c848c8dd5aL;
  expected_validity |= StackFrameRISCV64::CONTEXT_VALID_A2;
  CheckWalk();
}

// Here we move the .cfa, but provide an explicit rule to recover the SP,
// so again there should be no change in the registers recovered.
TEST_F(CFI, At4005) {
  Label frame1_sp = expected.sp;
  stack_section
      .D64(0xdd5a48c848c8dd5aL) // saved a2 (even though it's not callee-saves)
      .D64(0xff3dfb81fb81ff3dL) // no longer saved s1
      .D64(0x34f3ebd1ebd134f3L) // no longer saved s2
      .D64(0xe11081128112e110L) // saved s0
      .D64(0x0000000040005510L) // return address
      .Mark(&frame1_sp);        // This effectively sets stack_section.start().
  raw_context.pc = 0x0000000040004005L;
  raw_context.a2 = 0xfb756319fb756319L;  // distinct callee a2
  expected.a2 = 0xdd5a48c848c8dd5aL;     // caller's a2
  expected_validity |= StackFrameRISCV64::CONTEXT_VALID_A2;
  CheckWalk();
}

// Here we provide an explicit rule for the PC, and have the saved .ra be
// bogus.
TEST_F(CFI, At4006) {
  Label frame1_sp = expected.sp;
  stack_section
      .D64(0x0000000040005510L) // saved pc
      .D64(0xdd5a48c848c8dd5aL) // saved a2 (even though it's not callee-saves)
      .D64(0xff3dfb81fb81ff3dL) // no longer saved s1
      .D64(0x34f3ebd1ebd134f3L) // no longer saved s2
      .D64(0xe11081128112e110L) // saved s0
      .D64(0xf8d157835783f8d1L) // .ra rule recovers this, which is garbage
      .Mark(&frame1_sp);        // This effectively sets stack_section.start().
  raw_context.pc = 0x0000000040004006L;
  raw_context.a2 = 0xfb756319fb756319L;  // distinct callee a2
  expected.a2 = 0xdd5a48c848c8dd5aL;     // caller's a2
  expected_validity |= StackFrameRISCV64::CONTEXT_VALID_A2;
  CheckWalk();
}

// Check that we reject rules that would cause the stack pointer to
// move in the wrong direction.
TEST_F(CFI, RejectBackwards) {
  raw_context.pc = 0x0000000040006000L;
  raw_context.sp = 0x0000000080000000L;
  raw_context.ra = 0x0000000040005510L;
  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV64 walker(&system_info, &raw_context, &stack_region,
                            &modules, &frame_symbolizer);
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(0U, modules_without_symbols.size());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(1U, frames->size());
}

// Check that we reject rules whose expressions' evaluation fails.
TEST_F(CFI, RejectBadExpressions) {
  raw_context.pc = 0x0000000040007000L;
  raw_context.sp = 0x0000000080000000L;
  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV64 walker(&system_info, &raw_context, &stack_region,
                            &modules, &frame_symbolizer);
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(0U, modules_without_symbols.size());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(1U, frames->size());
}
