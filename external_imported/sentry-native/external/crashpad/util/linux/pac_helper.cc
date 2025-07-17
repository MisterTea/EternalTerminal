// Copyright 2023 The Crashpad Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/linux/pac_helper.h"

#if defined(__has_feature)
#define CRASHPAD_HAS_FEATURE(x) __has_feature(x)
#else
#define CRASHPAD_HAS_FEATURE(x) 0
#endif

#if CRASHPAD_HAS_FEATURE(ptrauth_intrinsics)
  #include <ptrauth.h>
#endif

#include "util/misc/address_types.h"

namespace crashpad {

VMAddress StripPACBits(VMAddress address) {
#if CRASHPAD_HAS_FEATURE(ptrauth_intrinsics)
    address = ptrauth_strip(address, ptrauth_key_function_pointer);
#elif defined(ARCH_CPU_ARM64)
    // Strip any pointer authentication bits that are assigned to the address.
    register uintptr_t x30 __asm("x30") = address;
  #ifdef SENTRY_MODIFIED
    asm("xpaclri" : "+r"(x30));
  #else
    // `xpaclri` is to be decoded as `NOP` if `FEAT_PAuth` is not implemented:
    // https://developer.arm.com/documentation/ddi0602/2024-12/Base-Instructions/XPACD--XPACI--XPACLRI--Strip-Pointer-Authentication-Code-#XPACLRI_HI_hints
    // which means we don't need any runtime checks on pre-ARMv8.3 or
    // environments where it is disabled.
    // Use `hint #7` as encoded here https://developer.arm.com/documentation/ddi0602/2024-12/Base-Instructions/HINT--Hint-instruction-
    // to support older toolchains with assemblers that don't know `xpaclri`:
    asm("hint #7" /* = xpaclri */ : "+r"(x30));
  #endif
    address = x30;
#endif
    return address;
}

}  // namespace crashpad

