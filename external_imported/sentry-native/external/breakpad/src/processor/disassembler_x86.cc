// Copyright 2010 Google LLC
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

// disassembler_x86.cc: simple x86 disassembler.
//
// Provides single step disassembly of x86 bytecode and flags instructions
// that utilize known bad register values.
//
// Author: Cris Neckar

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include "processor/disassembler_x86.h"

#include <string.h>

namespace google_breakpad {

DisassemblerX86::DisassemblerX86(const uint8_t* bytecode,
                                 uint32_t size,
                                 uint32_t virtual_address) :
                                     bytecode_(bytecode),
                                     size_(size),
                                     virtual_address_(virtual_address),
                                     current_byte_offset_(0),
                                     current_inst_offset_(0),
                                     instr_valid_(false),
                                     register_valid_(false),
                                     pushed_bad_value_(false),
                                     end_of_block_(false),
                                     flags_(0) {
  x86_init(opt_none, nullptr, nullptr);
}

DisassemblerX86::~DisassemblerX86() {
  if (instr_valid_)
    x86_oplist_free(&current_instr_);

  x86_cleanup();
}

uint32_t DisassemblerX86::NextInstruction() {
  if (instr_valid_)
    x86_oplist_free(&current_instr_);

  if (current_byte_offset_ >= size_) {
    instr_valid_ = false;
    return 0;
  }
  uint32_t instr_size = 0;
  instr_size = x86_disasm((unsigned char*)bytecode_, size_,
                          virtual_address_, current_byte_offset_,
                          &current_instr_);
  if (instr_size == 0) {
    instr_valid_ = false;
    return 0;
  }

  current_byte_offset_ += instr_size;
  current_inst_offset_++;
  instr_valid_ = x86_insn_is_valid(&current_instr_);
  if (!instr_valid_)
    return 0;

  if (current_instr_.type == insn_return)
    end_of_block_ = true;
  x86_op_t* src = x86_get_src_operand(&current_instr_);
  x86_op_t* dest = x86_get_dest_operand(&current_instr_);

  if (register_valid_) {
    switch (current_instr_.group) {
      // Flag branches based off of bad registers and calls that occur
      // after pushing bad values.
      case insn_controlflow:
        switch (current_instr_.type) {
          case insn_jmp:
          case insn_jcc:
          case insn_call:
          case insn_callcc:
            if (dest) {
              switch (dest->type) {
                case op_expression:
                  if (dest->data.expression.base.id == bad_register_.id)
                    flags_ |= DISX86_BAD_BRANCH_TARGET;
                  break;
                case op_register:
                  if (dest->data.reg.id == bad_register_.id)
                    flags_ |= DISX86_BAD_BRANCH_TARGET;
                  break;
                default:
                  if (pushed_bad_value_ &&
                      (current_instr_.type == insn_call ||
                      current_instr_.type == insn_callcc))
                    flags_ |= DISX86_BAD_ARGUMENT_PASSED;
                  break;
              }
            }
            break;
          default:
            break;
        }
        break;

      // Flag block data operations that use bad registers for src or dest.
      case insn_string:
        if (dest && dest->type == op_expression &&
            dest->data.expression.base.id == bad_register_.id)
          flags_ |= DISX86_BAD_BLOCK_WRITE;
        if (src && src->type == op_expression &&
            src->data.expression.base.id == bad_register_.id)
          flags_ |= DISX86_BAD_BLOCK_READ;
        break;

      // Flag comparisons based on bad data.
      case insn_comparison:
        if ((dest && dest->type == op_expression &&
            dest->data.expression.base.id == bad_register_.id) ||
            (src && src->type == op_expression &&
            src->data.expression.base.id == bad_register_.id) ||
            (dest && dest->type == op_register &&
            dest->data.reg.id == bad_register_.id) ||
            (src && src->type == op_register &&
            src->data.reg.id == bad_register_.id))
          flags_ |= DISX86_BAD_COMPARISON;
        break;

      // Flag any other instruction which derefs a bad register for
      // src or dest.
      default:
        if (dest && dest->type == op_expression &&
            dest->data.expression.base.id == bad_register_.id)
          flags_ |= DISX86_BAD_WRITE;
        if (src && src->type == op_expression &&
            src->data.expression.base.id == bad_register_.id)
          flags_ |= DISX86_BAD_READ;
        break;
    }
  }

  // When a register is marked as tainted check if it is pushed.
  // TODO(cdn): may also want to check for MOVs into EBP offsets.
  if (register_valid_ && dest && current_instr_.type == insn_push) {
    switch (dest->type) {
      case op_expression:
        if (dest->data.expression.base.id == bad_register_.id ||
            dest->data.expression.index.id == bad_register_.id)
          pushed_bad_value_ = true;
        break;
      case op_register:
        if (dest->data.reg.id == bad_register_.id)
          pushed_bad_value_ = true;
        break;
      default:
        break;
    }
  }

  // Check if a tainted register value is clobbered.
  // For conditional MOVs and XCHGs assume that
  // there is a hit.
  if (register_valid_) {
    switch (current_instr_.type) {
      case insn_xor:
        if (src && src->type == op_register &&
            dest && dest->type == op_register &&
            src->data.reg.id == bad_register_.id &&
            src->data.reg.id == dest->data.reg.id)
          register_valid_ = false;
        break;
      case insn_pop:
      case insn_mov:
      case insn_movcc:
        if (dest && dest->type == op_register &&
            dest->data.reg.id == bad_register_.id)
          register_valid_ = false;
        break;
      case insn_popregs:
        register_valid_ = false;
        break;
      case insn_xchg:
      case insn_xchgcc:
        if (dest && dest->type == op_register &&
            src && src->type == op_register) {
          if (dest->data.reg.id == bad_register_.id)
            memcpy(&bad_register_, &src->data.reg, sizeof(x86_reg_t));
          else if (src->data.reg.id == bad_register_.id)
            memcpy(&bad_register_, &dest->data.reg, sizeof(x86_reg_t));
        }
        break;
      default:
        break;
    }
  }

  return instr_size;
}

bool DisassemblerX86::setBadRead() {
  if (!instr_valid_)
    return false;

  x86_op_t* operand = x86_get_src_operand(&current_instr_);
  if (!operand || operand->type != op_expression)
    return false;

  memcpy(&bad_register_, &operand->data.expression.base,
         sizeof(x86_reg_t));
  register_valid_ = true;
  return true;
}

bool DisassemblerX86::setBadWrite() {
  if (!instr_valid_)
    return false;

  x86_op_t* operand = x86_get_dest_operand(&current_instr_);
  if (!operand || operand->type != op_expression)
    return false;

  memcpy(&bad_register_, &operand->data.expression.base,
         sizeof(x86_reg_t));
  register_valid_ = true;
  return true;
}

}  // namespace google_breakpad
