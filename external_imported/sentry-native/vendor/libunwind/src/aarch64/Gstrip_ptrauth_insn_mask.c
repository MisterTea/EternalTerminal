/* libunwind - a platform-independent unwind library

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#include "unwind_i.h"

static unw_word_t aarch64_strip_pac_remote (unw_accessors_t *a, unw_addr_space_t as, void *arg, unw_word_t old_ip)
{
  if (a->ptrauth_insn_mask)
    {
      unw_word_t ip, insn_mask;

      insn_mask = a->ptrauth_insn_mask (as, arg);
      ip = old_ip & (~insn_mask);

      Debug (15, "stripping pac from address, before: %lx, after: %lx\n", old_ip, ip);
      return ip;
    }
  else
    {
      Debug (15, "return address %lx might be signed, but no means to obtain mask\n", old_ip);
      return old_ip;
    }
}

static unw_word_t aarch64_strip_pac_local (unw_word_t in_addr)
{
  unw_word_t out_addr = in_addr;

#if defined(__aarch64__) && !defined(UNW_REMOTE_ONLY)
  // Strip the PAC with XPACLRI instruction
  register unsigned long long x30 __asm__("x30") = in_addr;
  __asm__("hint 0x7" : "+r" (x30));
  out_addr = x30;
#endif

  return out_addr;
}

HIDDEN unw_word_t tdep_strip_ptrauth_insn_mask (unw_cursor_t *cursor, unw_word_t ip)
{
  struct cursor *c = (struct cursor *) cursor;
  struct dwarf_cursor *d = &c->dwarf;
  unw_addr_space_t as = d->as;
  void *as_arg = d->as_arg;
  unw_accessors_t *a;
  unw_word_t stripped;

  if (as != unw_local_addr_space)
    {
      a = unw_get_accessors_int (as);
      stripped = aarch64_strip_pac_remote (a, as, as_arg, ip);
    }
  else
    {
      stripped = aarch64_strip_pac_local (ip);
    }

  Debug (16, "stripped PAC; oldip=0x%lx ip=0x%lx\n", ip, stripped);

  return stripped;
}
