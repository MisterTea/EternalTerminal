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
  // be static asserts but it is not avalible in older C versions.
#define kInvalidTimer 9999
  assert(kInvalidTimer != ITIMER_REAL);
  assert(kInvalidTimer != ITIMER_VIRTUAL);
  assert(kInvalidTimer != ITIMER_PROF);

  // Invalid timer returns EINVAL.
  assert(sys_setitimer(kInvalidTimer, NULL, NULL) == -1);
  assert(errno == EINVAL);

  const int kSignal = SIGALRM;
  const size_t kSigsetSize = sizeof(struct kernel_sigset_t);

  // Block SIGALRM.
  struct kernel_sigset_t sigalarm_only;
  struct kernel_sigset_t old_sigset;
  assert(sys_sigemptyset(&sigalarm_only) == 0);
  assert(sys_sigaddset(&sigalarm_only, kSignal) == 0);
  assert(sys_rt_sigprocmask(SIG_BLOCK, &sigalarm_only, &old_sigset,
                            kSigsetSize) == 0);

  // Set up a real time timer.
  struct kernel_itimerval new_itimer = {};
  const long kIntervalUSec = 123;
  new_itimer.it_interval.tv_sec = 0;
  new_itimer.it_interval.tv_usec = kIntervalUSec;
  new_itimer.it_value = new_itimer.it_interval;
  assert(sys_setitimer(ITIMER_REAL, &new_itimer, NULL) == 0);

  // Wait for alarm.
  struct timespec timeout;
  const unsigned long kNanoSecsPerSec = 1000000000;
  const unsigned long kNanoSecsPerMicroSec = 1000;

  // Use a timeout 3 times of the timer interval.
  unsigned long duration_ns = kIntervalUSec * kNanoSecsPerMicroSec * 3;
  timeout.tv_sec = duration_ns / kNanoSecsPerSec ;
  timeout.tv_nsec = duration_ns % kNanoSecsPerSec;

  int sig;
  do {
    sig = sys_sigtimedwait(&sigalarm_only, NULL, &timeout);
  } while (sig == -1 && errno == EINTR);
  assert(sig == kSignal);

  // Disable timer, check saving of old timer value.
  struct kernel_itimerval empty_itimer = {};
  struct kernel_itimerval old_itimer;
  empty_itimer.it_interval.tv_sec = 0;
  empty_itimer.it_interval.tv_usec = 0;
  empty_itimer.it_value = empty_itimer.it_interval;

  assert(sys_setitimer(ITIMER_REAL, &empty_itimer, &old_itimer) == 0);
  assert(kernel_timeval_eq(&old_itimer.it_interval, &new_itimer.it_interval));

  return 0;
}
