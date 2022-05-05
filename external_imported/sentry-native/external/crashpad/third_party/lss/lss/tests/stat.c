/* Copyright 2021, Google Inc.  All rights reserved.
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
 *     * Neither the name of Google Inc. nor the names of its
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
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "test_skel.h"

int main(int argc, char *argv[]) {
  int exit_status = 0;

  // Get two unique paths to play with.
  char foo[] = "tempfile.XXXXXX";
  int fd_foo = mkstemp(foo);
  assert(fd_foo != -1);

  // Make sure it exists.
  assert(access(foo, F_OK) == 0);

  // Make sure sys_stat() and a libc stat() implementation return the same
  // information.
  struct stat libc_stat;
  assert(stat(foo, &libc_stat) == 0);

  struct kernel_stat raw_stat;
  // We need to check our stat syscall for EOVERFLOW, as sometimes the integer
  // types used in the stat structures are too small to fit the actual value.
  // E.g. on some systems st_ino is 32-bit, but some filesystems have 64-bit
  // inodes.
  int rc = sys_stat(foo, &raw_stat);
  if (rc < 0 && errno == EOVERFLOW) {
    // Bail out since we had an overflow in the stat structure.
    exit_status = SKIP_TEST_EXIT_STATUS;
    goto cleanup;
  }
  assert(rc == 0);

  assert(libc_stat.st_ino == raw_stat.st_ino);


cleanup:
  sys_unlink(foo);
  return exit_status;
}
