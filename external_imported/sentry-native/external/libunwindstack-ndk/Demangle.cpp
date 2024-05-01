/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cxxabi.h>
#include <stdlib.h>

#include <string>

#ifdef SENTRY_REMOVED
#include <rustc_demangle.h>
#endif //SENTRY_REMOVED

#include <unwindstack/Demangle.h>

namespace unwindstack {

std::string DemangleNameIfNeeded(const std::string& name) {
  if (name.length() < 2 || name[0] != '_') {
    return name;
  }

  char* demangled_str = nullptr;
  if (name[1] == 'Z') {
    // Try to demangle C++ name.
    demangled_str = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, nullptr);
#ifdef SENTRY_REMOVED
  } else if (name[1] == 'R') {
    // Try to demangle rust name.
    demangled_str = rustc_demangle(name.c_str(), nullptr, nullptr, nullptr);
#endif //SENTRY_REMOVED
  }

  if (demangled_str == nullptr) {
    return name;
  }

  std::string demangled_name(demangled_str);
  free(demangled_str);
  return demangled_name;
}

}  // namespace unwindstack
