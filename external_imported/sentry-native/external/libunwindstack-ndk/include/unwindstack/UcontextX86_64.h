/*
 * Copyright (C) 2016 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _LIBUNWINDSTACK_UCONTEXT_X86_64_H
#define _LIBUNWINDSTACK_UCONTEXT_X86_64_H

#include <stdint.h>

#include <unwindstack/MachineX86_64.h>

namespace unwindstack {

struct x86_64_stack_t {
  uint64_t ss_sp;    // void __user*
  int32_t ss_flags;  // int
  int32_t pad;
  uint64_t ss_size;  // size_t
};

struct x86_64_mcontext_t {
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  uint64_t rdi;
  uint64_t rsi;
  uint64_t rbp;
  uint64_t rbx;
  uint64_t rdx;
  uint64_t rax;
  uint64_t rcx;
  uint64_t rsp;
  uint64_t rip;
  uint64_t efl;
  uint64_t csgsfs;
  uint64_t err;
  uint64_t trapno;
  uint64_t oldmask;
  uint64_t cr2;
  // Only care about the registers, skip everything else.
};

struct x86_64_ucontext_t {
  uint64_t uc_flags;  // unsigned long
  uint64_t uc_link;   // struct ucontext*
  x86_64_stack_t uc_stack;
  x86_64_mcontext_t uc_mcontext;
  // Nothing else is used, so don't define it.
};

}  // namespace unwindstack

#endif  // _LIBUNWINDSTACK_UCONTEXT_X86_64_H
