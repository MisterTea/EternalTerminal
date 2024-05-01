// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/scoped_file.h"

#include <stdio.h>

#include "base/logging.h"

#if BUILDFLAG(IS_POSIX)
#include <unistd.h>

#include "base/check.h"
#include "base/posix/eintr_wrapper.h"
#endif

namespace base {
namespace internal {

#if BUILDFLAG(IS_POSIX)
void ScopedFDCloseTraits::Free(int fd) {
  PCHECK(IGNORE_EINTR(close(fd)) == 0);
}
#endif  // BUILDFLAG(IS_POSIX)

void ScopedFILECloser::operator()(FILE* file) const {
  if (file) {
    if (fclose(file) < 0) {
      PLOG(ERROR) << "fclose";
    }
  }
}

}  // namespace internal
}  // namespace base
