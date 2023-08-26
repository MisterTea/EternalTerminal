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

// disassembler_objdump.: Disassembler that invokes objdump for disassembly.
//
// Author: Mark Brand

#include "processor/disassembler_objdump.h"

#include <unistd.h>
#include <sys/wait.h>

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <sstream>
#include <vector>

#include "common/linux/eintr_wrapper.h"
#include "common/linux/scoped_pipe.h"
#include "common/linux/scoped_tmpfile.h"
#include "processor/logging.h"

namespace google_breakpad {
namespace {

const size_t kMaxX86InstructionLength = 15;

bool IsInstructionPrefix(const string& token) {
  if (token == "lock" || token == "rep" || token == "repz" ||
      token == "repnz") {
    return true;
  }
  return false;
}

bool IsOperandSize(const string& token) {
  if (token == "BYTE" || token == "WORD" || token == "DWORD" ||
      token == "QWORD" || token == "PTR") {
    return true;
  }
  return false;
}

bool GetSegmentAddressX86(const DumpContext& context, string segment_name,
                          uint64_t& address) {
  if (segment_name == "ds") {
    address = context.GetContextX86()->ds;
  } else if (segment_name == "es") {
    address = context.GetContextX86()->es;
  } else if (segment_name == "fs") {
    address = context.GetContextX86()->fs;
  } else if (segment_name == "gs") {
    address = context.GetContextX86()->gs;
  } else {
    BPLOG(ERROR) << "Unsupported segment register: " << segment_name;
    return false;
  }

  return true;
}

bool GetSegmentAddressAMD64(const DumpContext& context, string segment_name,
                            uint64_t& address) {
  if (segment_name == "ds") {
    address = 0;
  } else if (segment_name == "es") {
    address = 0;
  } else {
    BPLOG(ERROR) << "Unsupported segment register: " << segment_name;
    return false;
  }

  return true;
}

bool GetSegmentAddress(const DumpContext& context, string segment_name,
                       uint64_t& address) {
  if (context.GetContextCPU() == MD_CONTEXT_X86) {
    return GetSegmentAddressX86(context, segment_name, address);
  } else if (context.GetContextCPU() == MD_CONTEXT_AMD64) {
    return GetSegmentAddressAMD64(context, segment_name, address);
  } else {
    BPLOG(ERROR) << "Unsupported architecture for GetSegmentAddress\n";
    return false;
  }
}

bool GetRegisterValueX86(const DumpContext& context, string register_name,
                         uint64_t& value) {
  if (register_name == "eax") {
    value = context.GetContextX86()->eax;
  } else if (register_name == "ebx") {
    value = context.GetContextX86()->ebx;
  } else if (register_name == "ecx") {
    value = context.GetContextX86()->ecx;
  } else if (register_name == "edx") {
    value = context.GetContextX86()->edx;
  } else if (register_name == "edi") {
    value = context.GetContextX86()->edi;
  } else if (register_name == "esi") {
    value = context.GetContextX86()->esi;
  } else if (register_name == "ebp") {
    value = context.GetContextX86()->ebp;
  } else if (register_name == "esp") {
    value = context.GetContextX86()->esp;
  } else if (register_name == "eip") {
    value = context.GetContextX86()->eip;
  } else {
    BPLOG(ERROR) << "Unsupported register: " << register_name;
    return false;
  }

  return true;
}

bool GetRegisterValueAMD64(const DumpContext& context, string register_name,
                           uint64_t& value) {
  if (register_name == "rax") {
    value = context.GetContextAMD64()->rax;
  } else if (register_name == "rbx") {
    value = context.GetContextAMD64()->rbx;
  } else if (register_name == "rcx") {
    value = context.GetContextAMD64()->rcx;
  } else if (register_name == "rdx") {
    value = context.GetContextAMD64()->rdx;
  } else if (register_name == "rdi") {
    value = context.GetContextAMD64()->rdi;
  } else if (register_name == "rsi") {
    value = context.GetContextAMD64()->rsi;
  } else if (register_name == "rbp") {
    value = context.GetContextAMD64()->rbp;
  } else if (register_name == "rsp") {
    value = context.GetContextAMD64()->rsp;
  } else if (register_name == "r8") {
    value = context.GetContextAMD64()->r8;
  } else if (register_name == "r9") {
    value = context.GetContextAMD64()->r9;
  } else if (register_name == "r10") {
    value = context.GetContextAMD64()->r10;
  } else if (register_name == "r11") {
    value = context.GetContextAMD64()->r11;
  } else if (register_name == "r12") {
    value = context.GetContextAMD64()->r12;
  } else if (register_name == "r13") {
    value = context.GetContextAMD64()->r13;
  } else if (register_name == "r14") {
    value = context.GetContextAMD64()->r14;
  } else if (register_name == "r15") {
    value = context.GetContextAMD64()->r15;
  } else if (register_name == "rip") {
    value = context.GetContextAMD64()->rip;
  } else {
    BPLOG(ERROR) << "Unsupported register: " << register_name;
    return false;
  }

  return true;
}

// Lookup the value of `register_name` in `context`, store it into `value` on
// success.
// Support for non-full-size registers not implemented, since we're only using
// this to evaluate address expressions.
bool GetRegisterValue(const DumpContext& context, string register_name,
                      uint64_t& value) {
  if (context.GetContextCPU() == MD_CONTEXT_X86) {
    return GetRegisterValueX86(context, register_name, value);
  } else if (context.GetContextCPU() == MD_CONTEXT_AMD64) {
    return GetRegisterValueAMD64(context, register_name, value);
  } else {
    BPLOG(ERROR) << "Unsupported architecture for GetRegisterValue\n";
    return false;
  }
}
}  // namespace

// static
bool DisassemblerObjdump::DisassembleInstruction(uint32_t cpu,
                                                 const uint8_t* raw_bytes,
                                                 unsigned int raw_bytes_len,
                                                 string& instruction) {
  // Always initialize outputs
  instruction = "";

  if (!raw_bytes || raw_bytes_len == 0) {
    // There's no need to perform any operation in this case, as there's
    // clearly no instruction there.
    return false;
  }

  string architecture;
  if (cpu == MD_CONTEXT_X86) {
    architecture = "i386";
  } else if (cpu == MD_CONTEXT_AMD64) {
    architecture = "i386:x86-64";
  } else {
    BPLOG(ERROR) << "Unsupported architecture.";
    return false;
  }

  // Create a temporary file for the raw instruction bytes to pass to
  // objdump, and write the bytes to the input file.
  ScopedTmpFile raw_bytes_file;
  if (!raw_bytes_file.InitData(raw_bytes, raw_bytes_len)) {
    BPLOG(ERROR) << "Failed creating temporary file.";
    return false;
  }

  // Create a pipe to use to read the disassembly back from objdump.
  ScopedPipe disassembly_pipe;
  if (!disassembly_pipe.Init()) {
    BPLOG(ERROR) << "Failed creating pipe for output.";
    return false;
  }

  pid_t child_pid = fork();
  if (child_pid < 0) {
    BPLOG(ERROR) << "Fork failed.";
    return false;
  }

  if (child_pid == 0) {
    // In the child process, set up the input and output file descriptors.
    if (dup2(raw_bytes_file.GetFd(), STDIN_FILENO) < 0 ||
        disassembly_pipe.Dup2WriteFd(STDOUT_FILENO) < 0 ||
        disassembly_pipe.Dup2WriteFd(STDERR_FILENO) < 0) {
      BPLOG(ERROR) << "Failed dup'ing file descriptors.";
      exit(-1);
    }

    // We need to close the read end of the pipe in the child process so that
    // when the parent closes it, the pipe is disconnected.
    disassembly_pipe.CloseReadFd();

    // We use "/proc/self/fd/0" here to allow objdump to parse an unnamed file,
    // since objdump does not have a mode to read from stdin. This cannot be
    // used with a pipe, since objdump requires that the input is a standard
    // file.
    execlp("objdump", "objdump", "-D", "--no-show-raw-insn", "-b", "binary",
           "-M", "intel", "-m", architecture.c_str(), "/proc/self/fd/0",
           nullptr);

    BPLOG(ERROR) << "Failed to exec objdump.";
    exit(-1);
  } else {
    // In the parent process, parse the objdump output.

    // Match the instruction line, from:
    //    0:        lock cmpxchg DWORD PTR [esi+0x10],eax
    // extract the string "lock cmpxchg DWORD PTR [esi+0x10],eax"
    std::regex instruction_regex(
        "^\\s+[0-9a-f]+:\\s+"  // "   0:"
        "((?:\\s*\\S*)+)$");   // "lock cmpxchg..."

    std::string line;
    std::smatch match;
    while (disassembly_pipe.ReadLine(line)) {
      if (std::regex_match(line, match, instruction_regex)) {
        instruction = match[1].str();
        break;
      }
    }

    // Close the read pipe so that objdump will exit (in case we broke out of
    // the loop above before reading all of the output).
    disassembly_pipe.CloseReadFd();

    // Now wait for objdump to exit.
    int status = 0;
    HANDLE_EINTR(waitpid(child_pid, &status, 0));

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      BPLOG(ERROR) << "objdump didn't run successfully.";
      return false;
    }

    if (instruction == "") {
      BPLOG(ERROR) << "Failed to find instruction in objdump output.";
      return false;
    }
  }

  return true;
}

// static
bool DisassemblerObjdump::TokenizeInstruction(const string& instruction,
                                              string& operation, string& dest,
                                              string& src) {
  // Always initialize outputs.
  operation = "";
  dest = "";
  src = "";

  // Split the instruction into tokens by either whitespace or comma.
  std::regex token_regex("((?:[^\\s,]+)|,)(?:\\s)*");
  std::sregex_iterator tokens_begin(instruction.begin(), instruction.end(),
                                    token_regex);

  bool found_comma = false;
  for (auto tokens_iter = tokens_begin; tokens_iter != std::sregex_iterator();
       ++tokens_iter) {
    auto token = (*tokens_iter)[1].str();
    if (operation.size() == 0) {
      if (IsInstructionPrefix(token))
        continue;
      operation = token;
    } else if (dest.size() == 0) {
      if (IsOperandSize(token))
        continue;
      dest = token;
    } else if (!found_comma) {
      if (token == ",") {
        found_comma = true;
      } else {
        BPLOG(ERROR) << "Failed to parse operands from objdump output, expected"
                        " comma but found \""
                     << token << "\"";
        return false;
      }
    } else if (src.size() == 0) {
      if (IsOperandSize(token))
        continue;
      src = token;
    } else {
      if (token == ",") {
        BPLOG(ERROR) << "Failed to parse operands from objdump output, found "
                        "unexpected comma after last operand.";
        return false;
      } else {
        // We just ignore other junk after the last operand unless it's a
        // comma, which would indicate we're probably still in the middle
        // of the operands and something has gone wrong
      }
    }
  }

  if (found_comma && src.size() == 0) {
    BPLOG(ERROR) << "Failed to parse operands from objdump output, found comma "
                    "but no src operand.";
    return false;
  }

  return true;
}

// static
bool DisassemblerObjdump::CalculateAddress(const DumpContext& context,
                                           const string& expression,
                                           uint64_t& address) {
  address = 0;

  // Extract the components of the expression.
  // fs:[esi+edi*4+0x80] -> ["fs", "esi", "edi", "4", "-", "0x80"]
  std::regex expression_regex(
      "^(?:(\\ws):)?"                // "fs:"
      "\\[(\\w+)"                    // "[esi"
      "(?:\\+(\\w+)(?:\\*(\\d+)))?"  // "+edi*4"
      "(?:([\\+-])(0x[0-9a-f]+))?"   // "-0x80"
      "\\]$");                       // "]"

  std::smatch match;
  if (!std::regex_match(expression, match, expression_regex) ||
      match.size() != 7) {
    return false;
  }

  string segment_name = match[1].str();
  string register_name = match[2].str();
  string index_name = match[3].str();
  string index_stride = match[4].str();
  string offset_sign = match[5].str();
  string offset = match[6].str();

  uint64_t segment_address = 0;
  uint64_t register_value = 0;
  uint64_t index_value = 0;
  uint64_t index_stride_value = 1;
  uint64_t offset_value = 0;

  if (segment_name.size() &&
      !GetSegmentAddress(context, segment_name, segment_address)) {
    return false;
  }

  if (!GetRegisterValue(context, register_name, register_value)) {
    return false;
  }

  if (index_name.size() &&
      !GetRegisterValue(context, index_name, index_value)) {
    return false;
  }

  if (index_stride.size()) {
    index_stride_value = strtoull(index_stride.c_str(), nullptr, 0);
  }

  if (offset.size()) {
    offset_value = strtoull(offset.c_str(), nullptr, 0);
  }

  address =
      segment_address + register_value + (index_value * index_stride_value);
  if (offset_sign == "+") {
    address += offset_value;
  } else if (offset_sign == "-") {
    address -= offset_value;
  }

  return true;
}

DisassemblerObjdump::DisassemblerObjdump(const uint32_t cpu,
                                         const MemoryRegion* memory_region,
                                         uint64_t address) {
  if (address < memory_region->GetBase() ||
      memory_region->GetBase() + memory_region->GetSize() <= address) {
    return;
  }

  uint8_t ip_bytes[kMaxX86InstructionLength] = {0};
  size_t ip_bytes_length;
  for (ip_bytes_length = 0; ip_bytes_length < kMaxX86InstructionLength;
       ++ip_bytes_length) {
    // We have to read byte-by-byte here, since we still want to try and
    // disassemble an instruction even if we don't have enough bytes.
    if (!memory_region->GetMemoryAtAddress(address + ip_bytes_length,
                                           &ip_bytes[ip_bytes_length])) {
      break;
    }
  }

  string instruction;
  if (!DisassembleInstruction(cpu, ip_bytes, kMaxX86InstructionLength,
                              instruction)) {
    return;
  }

  if (!TokenizeInstruction(instruction, operation_, dest_, src_)) {
    return;
  }
}

bool DisassemblerObjdump::CalculateSrcAddress(const DumpContext& context,
                                              uint64_t& address) {
  return CalculateAddress(context, src_, address);
}

bool DisassemblerObjdump::CalculateDestAddress(const DumpContext& context,
                                               uint64_t& address) {
  return CalculateAddress(context, dest_, address);
}

}  // namespace google_breakpad