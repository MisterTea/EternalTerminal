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

/* stackwalker_riscv_unittest.cc: Unit tests for StackwalkerRISCV class.
 *
 * Author: Iacopo Colonnelli
 */

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
#include "processor/stackwalker_riscv.h"
#include "processor/windows_frame_info.h"

using google_breakpad::BasicSourceLineResolver;
using google_breakpad::CallStack;
using google_breakpad::CodeModule;
using google_breakpad::StackFrameSymbolizer;
using google_breakpad::StackFrame;
using google_breakpad::StackFrameRISCV;
using google_breakpad::Stackwalker;
using google_breakpad::StackwalkerRISCV;
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

class StackwalkerRISCVFixture {
public:
  StackwalkerRISCVFixture()
      : stack_section(kLittleEndian),
      // Give the two modules reasonable standard locations and names
      // for tests to play with.
        module1(0x40000000, 0x10000, "module1", "version1"),
        module2(0x50000000, 0x10000, "module2", "version2") {
    // Identify the system as an iOS system.
    system_info.os = "iOS";
    system_info.os_short = "ios";
    system_info.cpu = "riscv";
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
  void BrandContext(MDRawContextRISCV *raw_context) {
    uint8_t x = 173;
    for (size_t i = 0; i < sizeof(*raw_context); i++)
      reinterpret_cast<uint8_t*>(raw_context)[i] = (x += 17);
  }

  SystemInfo system_info;
  MDRawContextRISCV raw_context;
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

class SanityCheck: public StackwalkerRISCVFixture, public Test { };

TEST_F(SanityCheck, NoResolver) {
  // Since the context's frame pointer is garbage, the stack walk will end after
  // the first frame.
  StackFrameSymbolizer frame_symbolizer(NULL, NULL);
  StackwalkerRISCV walker(&system_info, &raw_context, &stack_region,
                          &modules, &frame_symbolizer);
  // This should succeed even without a resolver or supplier.
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(0U, modules_without_symbols.size());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(1U, frames->size());
  StackFrameRISCV *frame = static_cast<StackFrameRISCV*>(frames->at(0));
  // Check that the values from the original raw context made it
  // through to the context in the stack frame.
  EXPECT_EQ(0, memcmp(&raw_context, &frame->context, sizeof(raw_context)));
}

class GetContextFrame: public StackwalkerRISCVFixture, public Test { };

// The stackwalker should be able to produce the context frame even
// without stack memory present.
TEST_F(GetContextFrame, NoStackMemory) {
  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV walker(&system_info, &raw_context, NULL, &modules,
                          &frame_symbolizer);
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(0U, modules_without_symbols.size());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(1U, frames->size());
  StackFrameRISCV *frame = static_cast<StackFrameRISCV*>(frames->at(0));
  // Check that the values from the original raw context made it
  // through to the context in the stack frame.
  EXPECT_EQ(0, memcmp(&raw_context, &frame->context, sizeof(raw_context)));
}

class GetCallerFrame: public StackwalkerRISCVFixture, public Test { };

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
      .Append(8, 0)                       // space

      .D32(0x40090000)                    // junk that's not
      .D32(0x60000000)                    // a return address

      .D32(return_address1)               // actual return address
      // frame 1
      .Mark(&frame1_sp)
      .Append(8, 0)                       // space

      .D32(0xF0000000)                    // more junk
      .D32(0x0000000D)

      .D32(return_address2)               // actual return address
      // frame 2
      .Mark(&frame2_sp)
      .Append(32, 0);                     // end of stack
  RegionFromSection();

  raw_context.pc = 0x40005510;
  raw_context.sp = stack_section.start().Value();

  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV walker(&system_info, &raw_context, &stack_region,
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
  ASSERT_EQ(3U, frames->size());

  StackFrameRISCV *frame0 = static_cast<StackFrameRISCV*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));

  StackFrameRISCV *frame1 = static_cast<StackFrameRISCV*>(frames->at(1));
  EXPECT_EQ(StackFrame::FRAME_TRUST_SCAN, frame1->trust);
  ASSERT_EQ((StackFrameRISCV::CONTEXT_VALID_PC |
             StackFrameRISCV::CONTEXT_VALID_SP),
             frame1->context_validity);
  EXPECT_EQ(return_address1, frame1->context.pc);
  EXPECT_EQ(frame1_sp.Value(), frame1->context.sp);

  StackFrameRISCV *frame2 = static_cast<StackFrameRISCV*>(frames->at(2));
  EXPECT_EQ(StackFrame::FRAME_TRUST_SCAN, frame2->trust);
  ASSERT_EQ((StackFrameRISCV::CONTEXT_VALID_PC |
             StackFrameRISCV::CONTEXT_VALID_SP),
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
      .Append(8, 0)                       // space

      .D32(0x40090000)                    // junk that's not
      .D32(0x60000000)                    // a return address

      .D32(0x40001000)                    // a couple of plausible addresses
      .D32(0x5000F000)                    // that are not within functions

      .D32(return_address)                // actual return address
      // frame 1
      .Mark(&frame1_sp)
      .Append(32, 0);                     // end of stack
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
  StackwalkerRISCV walker(&system_info, &raw_context, &stack_region,
                          &modules, &frame_symbolizer);
  vector<const CodeModule*> modules_without_symbols;
  vector<const CodeModule*> modules_with_corrupt_symbols;
  ASSERT_TRUE(walker.Walk(&call_stack, &modules_without_symbols,
              &modules_with_corrupt_symbols));
  ASSERT_EQ(0U, modules_without_symbols.size());
  ASSERT_EQ(0U, modules_with_corrupt_symbols.size());
            frames = call_stack.frames();
  ASSERT_EQ(2U, frames->size());

  StackFrameRISCV *frame0 = static_cast<StackFrameRISCV*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));
  EXPECT_EQ("monotreme", frame0->function_name);
  EXPECT_EQ(0x40000100UL, frame0->function_base);

  StackFrameRISCV *frame1 = static_cast<StackFrameRISCV*>(frames->at(1));
  EXPECT_EQ(StackFrame::FRAME_TRUST_SCAN, frame1->trust);
  ASSERT_EQ((StackFrameRISCV::CONTEXT_VALID_PC |
             StackFrameRISCV::CONTEXT_VALID_SP),
             frame1->context_validity);
  EXPECT_EQ(return_address, frame1->context.pc);
  EXPECT_EQ(frame1_sp.Value(), frame1->context.sp);
  EXPECT_EQ("marsupial", frame1->function_name);
  EXPECT_EQ(0x50000100UL, frame1->function_base);
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
      .Append(16, 0)                      // space

      .D32(0x40090000)                    // junk that's not
      .D32(0x60000000)                    // a return address

      .Append(48, 0)                      // more space

      .D32(return_address1)               // actual return address
      // frame 1
      .Mark(&frame1_sp)
      .Append(16, 0)                      // space

      .D32(0xF0000000)                    // more junk
      .D32(0x0000000D)

      .Append(168, 0)                     // more space

      .D32(return_address2)               // actual return address
      // (won't be found)
      // frame 2
      .Mark(&frame2_sp)
      .Append(32, 0);                     // end of stack
  RegionFromSection();

  raw_context.pc = 0x40005510;
  raw_context.sp = stack_section.start().Value();

  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV walker(&system_info, &raw_context, &stack_region,
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

  StackFrameRISCV *frame0 = static_cast<StackFrameRISCV*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));

  StackFrameRISCV *frame1 = static_cast<StackFrameRISCV*>(frames->at(1));
  EXPECT_EQ(StackFrame::FRAME_TRUST_SCAN, frame1->trust);
  ASSERT_EQ((StackFrameRISCV::CONTEXT_VALID_PC |
             StackFrameRISCV::CONTEXT_VALID_SP),
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
      .Append(8, 0)                       // space

      .D32(0x40090000)                    // junk that's not
      .D32(0x60000000)                    // a return address

      .D32(return_address1)               // actual return address
      // frame 1
      .Mark(&frame1_sp)
      .Append(8, 0)                       // space

      .D32(0xF0000000)                    // more junk
      .D32(0x0000000D)

      .D32(return_address2)               // actual return address
      // frame 2
      .Mark(&frame2_sp)
      .Append(32, 0);                     // end of stack
  RegionFromSection();

  raw_context.pc = 0x40005510;
  raw_context.sp = stack_section.start().Value();

  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV walker(&system_info, &raw_context, &stack_region,
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

  StackFrameRISCV *frame0 = static_cast<StackFrameRISCV*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));
}

class GetFramesByFramePointer:
    public StackwalkerRISCVFixture,
    public Test { };

TEST_F(GetFramesByFramePointer, OnlyFramePointer) {
  stack_section.start() = 0x80000000;
  uint64_t return_address1 = 0x50000100;
  uint64_t return_address2 = 0x50000900;
  Label frame1_sp, frame2_sp;
  Label frame1_fp, frame2_fp;
  stack_section
      // frame 0
      .Append(32, 0)           // Whatever values on the stack.
      .D32(0x0000000D)         // junk that's not
      .D32(0xF0000000)         // a return address.

      .Mark(&frame1_fp)        // Next fp will point to the next value.
      .D32(frame2_fp)          // Save current frame pointer.
      .D32(return_address2)    // Save current link register.
      .Mark(&frame1_sp)

      // frame 1
      .Append(32, 0)           // Whatever values on the stack.
      .D32(0x0000000D)         // junk that's not
      .D32(0xF0000000)         // a return address.

      .Mark(&frame2_fp)
      .D32(0)
      .D32(0)
      .Mark(&frame2_sp)

      // frame 2
      .Append(32, 0)           // Whatever values on the stack.
      .D32(0x0000000D)         // junk that's not
      .D32(0xF0000000);        // a return address.
  RegionFromSection();


  raw_context.pc = 0x40005510;
  raw_context.ra = return_address1;
  raw_context.s0 = frame1_fp.Value();
  raw_context.sp = stack_section.start().Value();

  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV walker(&system_info, &raw_context,
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

  StackFrameRISCV *frame0 = static_cast<StackFrameRISCV*>(frames->at(0));
  EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
  ASSERT_EQ(StackFrameRISCV::CONTEXT_VALID_ALL,
            frame0->context_validity);
  EXPECT_EQ(0, memcmp(&raw_context, &frame0->context, sizeof(raw_context)));

  StackFrameRISCV *frame1 = static_cast<StackFrameRISCV*>(frames->at(1));
  EXPECT_EQ(StackFrame::FRAME_TRUST_FP, frame1->trust);
  ASSERT_EQ((StackFrameRISCV::CONTEXT_VALID_PC |
             StackFrameRISCV::CONTEXT_VALID_RA |
             StackFrameRISCV::CONTEXT_VALID_S0 |
             StackFrameRISCV::CONTEXT_VALID_SP),
             frame1->context_validity);
  EXPECT_EQ(return_address1, frame1->context.pc);
  EXPECT_EQ(return_address2, frame1->context.ra);
  EXPECT_EQ(frame1_sp.Value(), frame1->context.sp);
  EXPECT_EQ(frame2_fp.Value(), frame1->context.s0);

  StackFrameRISCV *frame2 = static_cast<StackFrameRISCV*>(frames->at(2));
  EXPECT_EQ(StackFrame::FRAME_TRUST_FP, frame2->trust);
  ASSERT_EQ((StackFrameRISCV::CONTEXT_VALID_PC |
             StackFrameRISCV::CONTEXT_VALID_RA |
             StackFrameRISCV::CONTEXT_VALID_S0 |
             StackFrameRISCV::CONTEXT_VALID_SP),
             frame2->context_validity);
  EXPECT_EQ(return_address2, frame2->context.pc);
  EXPECT_EQ(0U, frame2->context.ra);
  EXPECT_EQ(frame2_sp.Value(), frame2->context.sp);
  EXPECT_EQ(0U, frame2->context.s0);
}

struct CFIFixture: public StackwalkerRISCVFixture {
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
                     "STACK CFI 4001 .cfa: sp 16 + .ra: .cfa -4 + ^"
                     " s1: .cfa -16 + ^ s2: .cfa -12 + ^ "
                     " s0: .cfa -8 + ^\n"
                     // Save s1..s4 in a1..a4: verify that we populate
                     // the youngest frame with all the values we have.
                     "STACK CFI 4002 s1: a1 s2: a2 s3: a3 s4: a4\n"
                     // Restore s1..s4. Save the non-callee-saves register a2.
                     "STACK CFI 4003 .cfa: sp 20 + a2: .cfa 20 - ^"
                     " s1: s1 s2: s2 s3: s3 s4: s4\n"
                     // Move the .cfa back eight bytes, to point at the return
                     // address, and restore the sp explicitly.
                     "STACK CFI 4005 .cfa: sp 16 + a2: .cfa 16 - ^"
                     " s0: .cfa 4 - ^ .ra: .cfa ^ sp: .cfa 4 +\n"
                     // Recover the PC explicitly from a new stack slot;
                     // provide garbage for the .ra.
                     "STACK CFI 4006 .cfa: sp 20 + pc: .cfa 20 - ^\n"

                     // The calling function.
                     "FUNC 5000 1000 10 epictetus\n"
                     // Mark it as end of stack.
                     "STACK CFI INIT 5000 1000 .cfa: 0 .ra: 0\n"

                     // A function whose CFI makes the stack pointer
                     // go backwards.
                     "FUNC 6000 1000 20 palinal\n"
                     "STACK CFI INIT 6000 1000 .cfa: sp 4 - .ra: ra\n"

                     // A function with CFI expressions that can't be
                     // evaluated.
                     "FUNC 7000 1000 20 rhetorical\n"
                     "STACK CFI INIT 7000 1000 .cfa: moot .ra: ambiguous\n");

    // Provide some distinctive values for the caller's registers.
    expected.pc  = 0x40005510;
    expected.sp  = 0x80000000;
    expected.s1  = 0xb5d55e68;
    expected.s2  = 0xebd134f3;
    expected.s3  = 0xa31e74bc;
    expected.s4  = 0x2dcb16b3;
    expected.s5  = 0x2ada2137;
    expected.s6  = 0xbbbb557d;
    expected.s7  = 0x48bf8ca7;
    expected.s8  = 0xab4621f0;
    expected.s9  = 0x32b71467;
    expected.s10 = 0xa673645f;
    expected.s11 = 0xa673645f;
    expected.s0  = 0x8112e110;

    // Expect CFI to recover all callee-saves registers. Since CFI is the
    // only stack frame construction technique we have, aside from the
    // context frame itself, there's no way for us to have a set of valid
    // registers smaller than this.
    expected_validity = (StackFrameRISCV::CONTEXT_VALID_PC  |
                         StackFrameRISCV::CONTEXT_VALID_SP  |
                         StackFrameRISCV::CONTEXT_VALID_S1  |
                         StackFrameRISCV::CONTEXT_VALID_S2  |
                         StackFrameRISCV::CONTEXT_VALID_S3  |
                         StackFrameRISCV::CONTEXT_VALID_S4  |
                         StackFrameRISCV::CONTEXT_VALID_S5  |
                         StackFrameRISCV::CONTEXT_VALID_S6  |
                         StackFrameRISCV::CONTEXT_VALID_S7  |
                         StackFrameRISCV::CONTEXT_VALID_S8  |
                         StackFrameRISCV::CONTEXT_VALID_S9  |
                         StackFrameRISCV::CONTEXT_VALID_S10 |
                         StackFrameRISCV::CONTEXT_VALID_S11 |
                         StackFrameRISCV::CONTEXT_VALID_S0);

    // By default, context frames provide all registers, as normal.
    context_frame_validity = StackFrameRISCV::CONTEXT_VALID_ALL;

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
    StackwalkerRISCV walker(&system_info, &raw_context, &stack_region,
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

    StackFrameRISCV *frame0 = static_cast<StackFrameRISCV*>(frames->at(0));
    EXPECT_EQ(StackFrame::FRAME_TRUST_CONTEXT, frame0->trust);
    ASSERT_EQ(context_frame_validity, frame0->context_validity);
    EXPECT_EQ("enchiridion", frame0->function_name);
    EXPECT_EQ(0x40004000U, frame0->function_base);

    StackFrameRISCV *frame1 = static_cast<StackFrameRISCV*>(frames->at(1));
    EXPECT_EQ(StackFrame::FRAME_TRUST_CFI, frame1->trust);
    ASSERT_EQ(expected_validity, frame1->context_validity);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_A2)
      EXPECT_EQ(expected.a2, frame1->context.a2);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S1)
      EXPECT_EQ(expected.s1, frame1->context.s1);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S2)
      EXPECT_EQ(expected.s2, frame1->context.s2);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S3)
      EXPECT_EQ(expected.s3, frame1->context.s3);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S4)
      EXPECT_EQ(expected.s4, frame1->context.s4);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S5)
      EXPECT_EQ(expected.s5, frame1->context.s5);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S6)
      EXPECT_EQ(expected.s6, frame1->context.s6);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S7)
      EXPECT_EQ(expected.s7, frame1->context.s7);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S8)
      EXPECT_EQ(expected.s8, frame1->context.s8);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S9)
      EXPECT_EQ(expected.s9, frame1->context.s9);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S10)
      EXPECT_EQ(expected.s10, frame1->context.s10);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S11)
      EXPECT_EQ(expected.s11, frame1->context.s11);
    if (expected_validity & StackFrameRISCV::CONTEXT_VALID_S0)
      EXPECT_EQ(expected.s0, frame1->context.s0);

    // We would never have gotten a frame in the first place if the SP
    // and PC weren't valid or ->instruction weren't set.
    EXPECT_EQ(expected.sp, frame1->context.sp);
    EXPECT_EQ(expected.pc, frame1->context.pc);
    EXPECT_EQ(expected.pc, frame1->instruction + 4);
    EXPECT_EQ("epictetus", frame1->function_name);
  }

  // The values we expect to find for the caller's registers.
  MDRawContextRISCV expected;

  // The validity mask for expected.
  int expected_validity;

  // The validity mask to impose on the context frame.
  int context_frame_validity;
};

class CFI: public CFIFixture, public Test { };

TEST_F(CFI, At4000) {
  stack_section.start() = expected.sp;
  raw_context.pc = 0x40004000;
  raw_context.ra = 0x40005510;
  CheckWalk();
}

TEST_F(CFI, At4001) {
  Label frame1_sp = expected.sp;
  stack_section
      .D32(0xb5d55e68)    // saved s1
      .D32(0xebd134f3)    // saved s2
      .D32(0x8112e110)    // saved s0
      .D32(0x40005510)    // return address
      .Mark(&frame1_sp);  // This effectively sets stack_section.start().
  raw_context.pc = 0x40004001;
  // distinct callee s1, s2 and s0
  raw_context.s1 = 0xa635adc9;
  raw_context.s2 = 0x35ac6231;
  raw_context.s0 = 0xbe145fc4;
  CheckWalk();
}

// As above, but unwind from a context that has only the PC and SP.
TEST_F(CFI, At4001LimitedValidity) {
  Label frame1_sp = expected.sp;
  stack_section
      .D32(0xb5d55e68)    // saved s1
      .D32(0xebd134f3)    // saved s2
      .D32(0x8112e110)    // saved s0
      .D32(0x40005510)    // return address
      .Mark(&frame1_sp);  // This effectively sets stack_section.start().
  context_frame_validity = StackFrameRISCV::CONTEXT_VALID_PC |
                           StackFrameRISCV::CONTEXT_VALID_SP;
  raw_context.pc = 0x40004001;
  raw_context.s0 = 0xbe145fc4;

  expected_validity = (StackFrameRISCV::CONTEXT_VALID_PC |
                       StackFrameRISCV::CONTEXT_VALID_SP |
                       StackFrameRISCV::CONTEXT_VALID_S0 |
                       StackFrameRISCV::CONTEXT_VALID_S1 |
                       StackFrameRISCV::CONTEXT_VALID_S2);
  CheckWalk();
}

TEST_F(CFI, At4002) {
  Label frame1_sp = expected.sp;
  stack_section
      .D32(0xfb81ff3d)    // no longer saved s1
      .D32(0xebd134f3)    // no longer saved s2
      .D32(0x8112e110)    // saved s0
      .D32(0x40005510)    // return address
      .Mark(&frame1_sp);  // This effectively sets stack_section.start().
  raw_context.pc = 0x40004002;
  raw_context.a1 = 0xb5d55e68;  // saved a1
  raw_context.a2 = 0xebd134f3;  // saved a2
  raw_context.a3 = 0xa31e74bc;  // saved a3
  raw_context.a4 = 0x2dcb16b3;  // saved a4
  raw_context.s1 = 0xa635adc9;  // distinct callee s1
  raw_context.s2 = 0x35ac6231;  // distinct callee s2
  raw_context.s3 = 0x4356ac45;  // distinct callee s3
  raw_context.s4 = 0x562f2561;  // distinct callee s4
  // distinct callee s0
  raw_context.s0 = 0xbe145fc4;
  CheckWalk();
}

TEST_F(CFI, At4003) {
  Label frame1_sp = expected.sp;
  stack_section
      .D32(0x48c8dd5a)    // saved a2 (even though it's not callee-saves)
      .D32(0xfb81ff3d)    // no longer saved s1
      .D32(0xebd134f3)    // no longer saved s2
      .D32(0x8112e110)    // saved s0
      .D32(0x40005510)    // return address
      .Mark(&frame1_sp);  // This effectively sets stack_section.start().
  raw_context.pc = 0x40004003;
  // distinct callee a2 and fp
  raw_context.a2 = 0xfb756319;
  raw_context.s0 = 0xbe145fc4;
  // caller's a2
  expected.a2 = 0x48c8dd5a;
  expected_validity |= StackFrameRISCV::CONTEXT_VALID_A2;
  CheckWalk();
}

// We have no new rule at module offset 0x4004, so the results here should
// be the same as those at module offset 0x4003.
TEST_F(CFI, At4004) {
  Label frame1_sp = expected.sp;
  stack_section
      .D32(0x48c8dd5a)    // saved a2 (even though it's not callee-saves)
      .D32(0xfb81ff3d)    // no longer saved s1
      .D32(0xebd134f3)    // no longer saved s2
      .D32(0x8112e110)    // saved s0
      .D32(0x40005510)    // return address
      .Mark(&frame1_sp);  // This effectively sets stack_section.start().
  raw_context.pc = 0x40004004;
  // distinct callee a2 and s0
  raw_context.a2 = 0xfb756319;
  raw_context.s0 = 0xbe145fc4;
  // caller's a2
  expected.a2 = 0x48c8dd5a;
  expected_validity |= StackFrameRISCV::CONTEXT_VALID_A2;
  CheckWalk();
}

// Here we move the .cfa, but provide an explicit rule to recover the SP,
// so again there should be no change in the registers recovered.
TEST_F(CFI, At4005) {
  Label frame1_sp = expected.sp;
  stack_section
      .D32(0x48c8dd5a)    // saved a2 (even though it's not callee-saves)
      .D32(0xfb81ff3d)    // no longer saved s1
      .D32(0xebd134f3)    // no longer saved s2
      .D32(0x8112e110)    // saved s0
      .D32(0x40005510)    // return address
      .Mark(&frame1_sp);  // This effectively sets stack_section.start().
  raw_context.pc = 0x40004005;
  raw_context.a2 = 0xfb756319;  // distinct callee a2
  expected.a2 = 0x48c8dd5a;     // caller's a2
  expected_validity |= StackFrameRISCV::CONTEXT_VALID_A2;
  CheckWalk();
}

// Here we provide an explicit rule for the PC, and have the saved .ra be
// bogus.
TEST_F(CFI, At4006) {
  Label frame1_sp = expected.sp;
  stack_section
      .D32(0x40005510)    // saved pc
      .D32(0x48c8dd5a)    // saved a2 (even though it's not callee-saves)
      .D32(0xfb81ff3d)    // no longer saved s1
      .D32(0xebd134f3)    // no longer saved s2
      .D32(0x8112e110)    // saved s0
      .D32(0x5783f8d1)    // .ra rule recovers this, which is garbage
      .Mark(&frame1_sp);  // This effectively sets stack_section.start().
  raw_context.pc = 0x40004006;
  raw_context.a2 = 0xfb756319;  // distinct callee a2
  expected.a2 = 0x48c8dd5a;     // caller's a2
  expected_validity |= StackFrameRISCV::CONTEXT_VALID_A2;
  CheckWalk();
}

// Check that we reject rules that would cause the stack pointer to
// move in the wrong direction.
TEST_F(CFI, RejectBackwards) {
  raw_context.pc = 0x40006000;
  raw_context.sp = 0x80000000;
  raw_context.ra = 0x40005510;
  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV walker(&system_info, &raw_context, &stack_region,
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
  raw_context.pc = 0x40007000;
  raw_context.sp = 0x80000000;
  StackFrameSymbolizer frame_symbolizer(&supplier, &resolver);
  StackwalkerRISCV walker(&system_info, &raw_context, &stack_region,
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
