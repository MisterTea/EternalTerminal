/*
* This file is part of libunwind.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "_UPT_internal.h"

#if defined(UNW_TARGET_AARCH64) && defined(NT_ARM_PAC_MASK)

unw_word_t _UPT_ptrauth_insn_mask (UNUSED unw_addr_space_t as, void *arg)
{
  struct UPT_info *ui = arg;
  pid_t pid = ui->pid;
  int ret;
  struct iovec iovec;
  uint64_t regset[2] = {0, 0};

  iovec.iov_base = &regset;
  iovec.iov_len = sizeof (regset);

  ret = ptrace (PTRACE_GETREGSET, pid, NT_ARM_PAC_MASK, &iovec);
  if (ret != 0)
    {
      Debug (12, "Failed to fetch ptrauth instruction mask");
      return 0;
    }

  // regset[0] => data_mask
  // regset[1] => insn_mask
  return regset[1];
}

#else

unw_word_t _UPT_ptrauth_insn_mask (UNUSED unw_addr_space_t as, UNUSED void *arg)
{
  return 0;
}

#endif

