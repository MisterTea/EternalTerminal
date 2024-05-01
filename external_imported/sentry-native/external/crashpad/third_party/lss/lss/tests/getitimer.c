/* Copyright 2022 Google LLC
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
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "test_skel.h"

int main(int argc, char *argv[]) {
  // We need an invalid timer value. The assert()'s below should
  // be static asserts but it is not available in older C versions.
#define kInvalidTimer 9999
  assert(kInvalidTimer != ITIMER_REAL);
  assert(kInvalidTimer != ITIMER_VIRTUAL);
  assert(kInvalidTimer != ITIMER_PROF);

  // This should fail with EINVAL.
  struct kernel_itimerval curr_itimer;
  assert(sys_getitimer(kInvalidTimer, &curr_itimer) == -1);
  assert(errno == EINVAL);

  // Create a read-only page.
  size_t page_size = getpagesize();
  void* read_only_page = sys_mmap(NULL, page_size, PROT_READ,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(read_only_page != MAP_FAILED);

  // This should fail with EFAULT.
  assert(sys_getitimer(ITIMER_REAL,
                       (struct kernel_itimerval*) read_only_page) == -1);
  assert(errno == EFAULT);

  // This should complete without an error.
  assert(sys_getitimer(ITIMER_REAL, &curr_itimer) == 0);

  // Set up a real time timer with very long interval and value so that
  // we do not need to handle SIGALARM in test.
  struct kernel_itimerval new_itimer;
  const time_t kIntervalSec = 60 * 60 * 24 * 365;  // One year.
  const long kIntervalUSec = 123;
  new_itimer.it_interval.tv_sec = kIntervalSec;
  new_itimer.it_interval.tv_usec = kIntervalUSec;
  new_itimer.it_value = new_itimer.it_interval;
  assert(sys_setitimer(ITIMER_REAL, &new_itimer, NULL) == 0);

  assert(sys_getitimer(ITIMER_REAL, &curr_itimer) == 0);
  assert(kernel_timeval_eq(&curr_itimer.it_interval, &new_itimer.it_interval));

  // Disable timer.
  struct kernel_itimerval empty_itimer;
  empty_itimer.it_interval.tv_sec = 0;
  empty_itimer.it_interval.tv_usec = 0;
  empty_itimer.it_value = empty_itimer.it_interval;
  assert(sys_setitimer(ITIMER_REAL, &empty_itimer, NULL) == 0);

  // We should read back an empty itimer.
  assert(sys_getitimer(ITIMER_REAL, &curr_itimer) == 0);
  assert(kernel_itimerval_eq(&curr_itimer, &empty_itimer));

  return 0;
}
