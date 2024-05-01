/* Copyright 2006 Google LLC
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google LLC nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

/* minidump_exception_linux.h: A definition of exception codes for
 * Linux
 *
 * (This is C99 source, please don't corrupt it with C++.)
 *
 * Author: Mark Mentovai
 * Split into its own file: Neal Sidhwaney */

#ifndef GOOGLE_BREAKPAD_COMMON_MINIDUMP_EXCEPTION_LINUX_H__
#define GOOGLE_BREAKPAD_COMMON_MINIDUMP_EXCEPTION_LINUX_H__

#include <stddef.h>

#include "google_breakpad/common/breakpad_types.h"

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#else
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30
#define SIGSYS 31
#endif

#ifndef SIGSTKFLT
#define SIGSTKFLT 990 /* 16 on x64 */
#endif
#ifndef SIGPWR
#define SIGPWR 991 /* 30 on x64 */
#endif

/* For (MDException).exception_code.  These values come from bits/signum.h.
 */
typedef enum {
  MD_EXCEPTION_CODE_LIN_SIGHUP = SIGHUP,   /* Hangup (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGINT = SIGINT,   /* Interrupt (ANSI) */
  MD_EXCEPTION_CODE_LIN_SIGQUIT = SIGQUIT, /* Quit (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGILL = SIGILL,   /* Illegal instruction (ANSI) */
  MD_EXCEPTION_CODE_LIN_SIGTRAP = SIGTRAP, /* Trace trap (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGABRT = SIGABRT, /* Abort (ANSI) */
  MD_EXCEPTION_CODE_LIN_SIGBUS = SIGBUS,   /* BUS error (4.2 BSD) */
  MD_EXCEPTION_CODE_LIN_SIGFPE = SIGFPE,   /* Floating-point exception (ANSI) */
  MD_EXCEPTION_CODE_LIN_SIGKILL = SIGKILL, /* Kill, unblockable (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGUSR1 = SIGUSR1, /* User-defined signal 1 (POSIX).  */
  MD_EXCEPTION_CODE_LIN_SIGSEGV = SIGSEGV, /* Segmentation violation (ANSI) */
  MD_EXCEPTION_CODE_LIN_SIGUSR2 = SIGUSR2, /* User-defined signal 2 (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGPIPE = SIGPIPE, /* Broken pipe (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGALRM = SIGALRM, /* Alarm clock (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGTERM = SIGTERM, /* Termination (ANSI) */
  MD_EXCEPTION_CODE_LIN_SIGSTKFLT = SIGSTKFLT, /* Stack faultd */
  MD_EXCEPTION_CODE_LIN_SIGCHLD =
      SIGCHLD, /* Child status has changed (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGCONT = SIGCONT, /* Continue (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGSTOP = SIGSTOP, /* Stop, unblockable (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGTSTP = SIGTSTP, /* Keyboard stop (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGTTIN =
      SIGTTIN, /* Background read from tty (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGTTOU = SIGTTOU, /* Background write to tty (POSIX) */
  MD_EXCEPTION_CODE_LIN_SIGURG = SIGURG,
  /* Urgent condition on socket (4.2 BSD) */
  MD_EXCEPTION_CODE_LIN_SIGXCPU = SIGXCPU, /* CPU limit exceeded (4.2 BSD) */
  MD_EXCEPTION_CODE_LIN_SIGXFSZ = SIGXFSZ,
  /* File size limit exceeded (4.2 BSD) */
  MD_EXCEPTION_CODE_LIN_SIGVTALRM =
      SIGVTALRM,                           /* Virtual alarm clock (4.2 BSD) */
  MD_EXCEPTION_CODE_LIN_SIGPROF = SIGPROF, /* Profiling alarm clock (4.2 BSD) */
  MD_EXCEPTION_CODE_LIN_SIGWINCH =
      SIGWINCH,                          /* Window size change (4.3 BSD, Sun) */
  MD_EXCEPTION_CODE_LIN_SIGIO = SIGIO,   /* I/O now possible (4.2 BSD) */
  MD_EXCEPTION_CODE_LIN_SIGPWR = SIGPWR, /* Power failure restart (System V) */
  MD_EXCEPTION_CODE_LIN_SIGSYS = SIGSYS, /* Bad system call */
  MD_EXCEPTION_CODE_LIN_DUMP_REQUESTED = 0xFFFFFFFF /* No exception,
                                                       dump requested. */
} MDExceptionCodeLinux;

/* For (MDException).exception_flags.  These values come from
 * asm-generic/siginfo.h.
 */
typedef enum {
  /* SIGILL */
  MD_EXCEPTION_FLAG_LIN_ILL_ILLOPC = 1,
  MD_EXCEPTION_FLAG_LIN_ILL_ILLOPN = 2,
  MD_EXCEPTION_FLAG_LIN_ILL_ILLADR = 3,
  MD_EXCEPTION_FLAG_LIN_ILL_ILLTRP = 4,
  MD_EXCEPTION_FLAG_LIN_ILL_PRVOPC = 5,
  MD_EXCEPTION_FLAG_LIN_ILL_PRVREG = 6,
  MD_EXCEPTION_FLAG_LIN_ILL_COPROC = 7,
  MD_EXCEPTION_FLAG_LIN_ILL_BADSTK = 8,

  /* SIGFPE */
  MD_EXCEPTION_FLAG_LIN_FPE_INTDIV = 1,
  MD_EXCEPTION_FLAG_LIN_FPE_INTOVF = 2,
  MD_EXCEPTION_FLAG_LIN_FPE_FLTDIV = 3,
  MD_EXCEPTION_FLAG_LIN_FPE_FLTOVF = 4,
  MD_EXCEPTION_FLAG_LIN_FPE_FLTUND = 5,
  MD_EXCEPTION_FLAG_LIN_FPE_FLTRES = 6,
  MD_EXCEPTION_FLAG_LIN_FPE_FLTINV = 7,
  MD_EXCEPTION_FLAG_LIN_FPE_FLTSUB = 8,

  /* SIGSEGV */
  MD_EXCEPTION_FLAG_LIN_SEGV_MAPERR = 1,
  MD_EXCEPTION_FLAG_LIN_SEGV_ACCERR = 2,
  MD_EXCEPTION_FLAG_LIN_SEGV_BNDERR = 3,
  MD_EXCEPTION_FLAG_LIN_SEGV_PKUERR = 4,
  MD_EXCEPTION_FLAG_LIN_SEGV_ACCADI = 5,
  MD_EXCEPTION_FLAG_LIN_SEGV_ADIDERR = 6,
  MD_EXCEPTION_FLAG_LIN_SEGV_ADIPERR = 7,
  MD_EXCEPTION_FLAG_LIN_SEGV_MTEAERR = 8,
  MD_EXCEPTION_FLAG_LIN_SEGV_MTESERR = 9,

  /* SIGBUS */
  MD_EXCEPTION_FLAG_LIN_BUS_ADRALN = 1,
  MD_EXCEPTION_FLAG_LIN_BUS_ADRERR = 2,
  MD_EXCEPTION_FLAG_LIN_BUS_OBJERR = 3,
  MD_EXCEPTION_FLAG_LIN_BUS_MCEERR_AR = 4,
  MD_EXCEPTION_FLAG_LIN_BUS_MCEERR_AO = 5,
} MDExceptionFlagLinux;

#endif /* GOOGLE_BREAKPAD_COMMON_MINIDUMP_EXCEPTION_LINUX_H__ */
