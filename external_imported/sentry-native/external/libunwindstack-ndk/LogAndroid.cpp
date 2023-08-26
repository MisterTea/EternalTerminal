/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

#define LOG_TAG "unwind"
#ifndef SENTRY_MODIFIED
#include <android/log.h>
#endif // SENTRY_MODIFIED

#ifdef SENTRY_REMOVED
#if defined(__BIONIC__)
#include <async_safe/log.h>
#endif
#endif // SENTRY_REMOVED
#include <android-base/stringprintf.h>

#include <unwindstack/Log.h>

namespace unwindstack {

namespace Log {

// Send the data to the log.
static void LogWithPriority(int priority, uint8_t indent, const char* format, va_list args) {
  std::string real_format;
  if (indent > 0) {
    real_format = android::base::StringPrintf("%*s%s", 2 * indent, " ", format);
  } else {
    real_format = format;
  }
#ifndef SENTRY_MODIFIED
  __android_log_vprint(priority, LOG_TAG, real_format.c_str(), args);
#endif // SENTRY_MODIFIED
}

void Info(const char* format, ...) {
  va_list args;
  va_start(args, format);
  LogWithPriority(ANDROID_LOG_INFO, 0, format, args);
  va_end(args);
}

void Info(uint8_t indent, const char* format, ...) {
  va_list args;
  va_start(args, format);
  LogWithPriority(ANDROID_LOG_INFO, indent, format, args);
  va_end(args);
}

void Error(const char* format, ...) {
  va_list args;
  va_start(args, format);
  LogWithPriority(ANDROID_LOG_ERROR, 0, format, args);
  va_end(args);
}

void AsyncSafe(const char*, ...) {}
#ifdef SENTRY_REMOVED
#if defined(__BIONIC__)
void AsyncSafe(const char* format, ...) {
  va_list args;
  va_start(args, format);
  async_safe_format_log_va_list(ANDROID_LOG_ERROR, "libunwindstack", format, args);
  va_end(args);
}
#else
void AsyncSafe(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  printf("\n");
  va_end(args);
}
#endif
#endif // SENTRY_REMOVED

}  // namespace Log

}  // namespace unwindstack
