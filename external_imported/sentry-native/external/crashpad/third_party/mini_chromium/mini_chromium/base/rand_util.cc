// Copyright 2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/rand_util.h"

#include <fcntl.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/check_op.h"
#include "base/files/file_util.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_FUCHSIA)
#include <zircon/syscalls.h>
#include "base/fuchsia/fuchsia_logging.h"
#elif BUILDFLAG(IS_POSIX)
#include "base/posix/eintr_wrapper.h"
#elif BUILDFLAG(IS_WIN)
#include <windows.h>

// #define needed to link in RtlGenRandom(), a.k.a. SystemFunction036.  See the
// "Community Additions" comment on MSDN here:
// http://msdn.microsoft.com/en-us/library/windows/desktop/aa387694.aspx
#define SystemFunction036 NTAPI SystemFunction036
#include <NTSecAPI.h>
#undef SystemFunction036

#endif  // BUILDFLAG(IS_WIN)

#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_FUCHSIA)

namespace {

int GetUrandomFDInternal() {
  int fd = HANDLE_EINTR(open("/dev/urandom", O_RDONLY | O_NOCTTY | O_CLOEXEC));
  PCHECK(fd >= 0) << "open /dev/urandom";
  return fd;
}

int GetUrandomFD() {
  static int fd = GetUrandomFDInternal();
  return fd;
}

}  // namespace

#endif  // BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_FUCHSIA)

namespace base {

uint64_t RandUint64() {
  uint64_t number;
  RandBytes(byte_span_from_ref(number));
  return number;
}

int RandInt(int min, int max) {
  DCHECK_LE(min, max);

  uint64_t range = static_cast<uint64_t>(max) - min + 1;
  int result = min + static_cast<int>(base::RandGenerator(range));
  DCHECK_GE(result, min);
  DCHECK_LE(result, max);

  return result;
}

uint64_t RandGenerator(uint64_t range) {
  DCHECK_GT(range, 0u);

  uint64_t max_acceptable_value =
      (std::numeric_limits<uint64_t>::max() / range) * range - 1;

  uint64_t value;
  do {
    value = base::RandUint64();
  } while (value > max_acceptable_value);

  return value % range;
}

double RandDouble() {
  static_assert(std::numeric_limits<double>::radix == 2,
                "otherwise use scalbn");
  static_assert(std::numeric_limits<double>::digits <
                std::numeric_limits<uint64_t>::digits,
                "integer type must be wider than floating-point mantissa");

  uint64_t random_bits = RandUint64();
  const int kMantissaBits = std::numeric_limits<double>::digits;

  uint64_t mantissa = random_bits & ((UINT64_C(1) << kMantissaBits) - 1);

  double result = std::ldexp(mantissa, -1 * kMantissaBits);

  DCHECK_GE(result, 0.0);
  DCHECK_LT(result, 1.0);

  return result;
}

void RandBytes(span<uint8_t> output) {
  if (output.empty()) {
    return;
  }

#if BUILDFLAG(IS_FUCHSIA)
  zx_cprng_draw(output.data(), output.size());
#elif BUILDFLAG(IS_POSIX)
  int fd = GetUrandomFD();
  bool success =
      ReadFromFD(fd, reinterpret_cast<char*>(output.data()), output.size());
  CHECK(success);
#elif BUILDFLAG(IS_WIN)
  while (!output.empty()) {
    const ULONG output_bytes_this_pass = static_cast<ULONG>(std::min(
        output.size(), static_cast<size_t>(std::numeric_limits<ULONG>::max())));
    const bool success =
        RtlGenRandom(output.data(), output_bytes_this_pass) != FALSE;
    CHECK(success);
    output = output.subspan(output_bytes_this_pass);
  }
#endif
}

std::string RandBytesAsString(size_t length) {
  if (length == 0) {
    return std::string();
  }

  std::string result(length, std::string::value_type());
  RandBytes(as_writable_bytes(make_span(result)));
  return result;
}

}  // namespace base
