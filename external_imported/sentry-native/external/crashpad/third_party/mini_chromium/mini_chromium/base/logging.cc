// Copyright 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/logging.h"

#include <stdio.h>
#include <stdlib.h>

#include <iomanip>
#include <ostream>

#if defined(OS_POSIX)
#include <paths.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "base/posix/safe_strerror.h"
#endif  // OS_POSIX

#if defined(OS_APPLE)
// In macOS 10.12 and iOS 10.0 and later ASL (Apple System Log) was deprecated
// in favor of OS_LOG (Unified Logging).
#include <AvailabilityMacros.h>
#if defined(OS_IOS)
#if !defined(__IPHONE_10_0) || __IPHONE_OS_VERSION_MIN_REQUIRED < __IPHONE_10_0
#define USE_ASL
#endif
#else  // !defined(OS_IOS)
#if !defined(MAC_OS_X_VERSION_10_12) || \
    MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_12
#define USE_ASL
#endif
#endif  // defined(OS_IOS)

#if defined(USE_ASL)
#include <asl.h>
#else
#include <os/log.h>
#endif  // USE_ASL

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>

#elif defined(OS_LINUX)
#include <sys/syscall.h>
#include <sys/types.h>
#elif defined(OS_WIN)
#include <intrin.h>
#include <windows.h>
#elif defined(OS_ANDROID)
#include <android/log.h>
#elif defined(OS_FUCHSIA)
#include <lib/syslog/global.h>
#endif

#include "base/cxx17_backports.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"

namespace logging {

namespace {

const char* const log_severity_names[] = {
  "INFO",
  "WARNING",
  "ERROR",
  "ERROR_REPORT",
  "FATAL"
};

LogMessageHandlerFunction g_log_message_handler = nullptr;

LoggingDestination g_logging_destination = LOG_DEFAULT;

}  // namespace

bool InitLogging(const LoggingSettings& settings) {
  DCHECK_EQ(settings.logging_dest & LOG_TO_FILE, 0u);

  g_logging_destination = settings.logging_dest;
  return true;
}

void SetLogMessageHandler(LogMessageHandlerFunction log_message_handler) {
  g_log_message_handler = log_message_handler;
}

LogMessageHandlerFunction GetLogMessageHandler() {
  return g_log_message_handler;
}

#if defined(OS_WIN)
std::string SystemErrorCodeToString(unsigned long error_code) {
  wchar_t msgbuf[256];
  DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                FORMAT_MESSAGE_MAX_WIDTH_MASK;
  DWORD len = FormatMessage(flags,
                            nullptr,
                            error_code,
                            0,
                            msgbuf,
                            static_cast<DWORD>(base::size(msgbuf)),
                            nullptr);
  if (len) {
    // Most system messages end in a period and a space. Remove the space if
    // it’s there, because the following StringPrintf() includes one.
    if (len >= 1 && msgbuf[len - 1] == ' ') {
      msgbuf[len - 1] = '\0';
    }
    return base::StringPrintf("%s (%u)",
                              base::WideToUTF8(msgbuf).c_str(), error_code);
  }
  return base::StringPrintf("Error %u while retrieving error %u",
                            GetLastError(),
                            error_code);
}
#endif  // OS_WIN

LogMessage::LogMessage(const char* function,
                       const char* file_path,
                       int line,
                       LogSeverity severity)
    : stream_(),
      file_path_(file_path),
      message_start_(0),
      line_(line),
      severity_(severity) {
  Init(function);
}

LogMessage::LogMessage(const char* function,
                       const char* file_path,
                       int line,
                       std::string* result)
    : stream_(),
      file_path_(file_path),
      message_start_(0),
      line_(line),
      severity_(LOG_FATAL) {
  Init(function);
  stream_ << "Check failed: " << *result << ". ";
  delete result;
}

LogMessage::~LogMessage() {
  stream_ << std::endl;
  std::string str_newline(stream_.str());

  if (g_log_message_handler &&
      g_log_message_handler(
          severity_, file_path_, line_, message_start_, str_newline)) {
    return;
  }

  if ((g_logging_destination & LOG_TO_STDERR)) {
    fprintf(stderr, "%s", str_newline.c_str());
    fflush(stderr);
  }

  if ((g_logging_destination & LOG_TO_SYSTEM_DEBUG_LOG) != 0) {
#if defined(OS_APPLE)
    const bool log_to_system = []() {
      struct stat stderr_stat;
      if (fstat(fileno(stderr), &stderr_stat) == -1) {
        return true;
      }
      if (!S_ISCHR(stderr_stat.st_mode)) {
        return false;
      }

      struct stat dev_null_stat;
      if (stat(_PATH_DEVNULL, &dev_null_stat) == -1) {
        return true;
      }

      return !S_ISCHR(dev_null_stat.st_mode) ||
             stderr_stat.st_rdev == dev_null_stat.st_rdev;
    }();

    if (log_to_system) {
      CFBundleRef main_bundle = CFBundleGetMainBundle();
      CFStringRef main_bundle_id_cf =
          main_bundle ? CFBundleGetIdentifier(main_bundle) : nullptr;

      std::string main_bundle_id_buf;
      const char* main_bundle_id = nullptr;

      if (main_bundle_id_cf) {
        main_bundle_id =
            CFStringGetCStringPtr(main_bundle_id_cf, kCFStringEncodingUTF8);
        if (!main_bundle_id) {
          // 1024 is from 10.10.5 CF-1153.18/CFBundle.c __CFBundleMainID__ (at
          // the point of use, not declaration).
          main_bundle_id_buf.resize(1024);
          if (!CFStringGetCString(main_bundle_id_cf,
                                  &main_bundle_id_buf[0],
                                  main_bundle_id_buf.size(),
                                  kCFStringEncodingUTF8)) {
            main_bundle_id_buf.clear();
          } else {
            main_bundle_id = &main_bundle_id_buf[0];
          }
        }
      }

#if defined(USE_ASL)
      // Use ASL when this might run on pre-10.12 systems. Unified Logging
      // (os_log) was introduced in 10.12.

      const class ASLClient {
       public:
        explicit ASLClient(const char* asl_facility)
            : client_(asl_open(nullptr, asl_facility, ASL_OPT_NO_DELAY)) {}

        ASLClient(const ASLClient&) = delete;
        ASLClient& operator=(const ASLClient&) = delete;

        ~ASLClient() { asl_close(client_); }

        aslclient get() const { return client_; }

       private:
        aslclient client_;
      } asl_client(main_bundle_id ? main_bundle_id : "com.apple.console");

      const class ASLMessage {
       public:
        ASLMessage() : message_(asl_new(ASL_TYPE_MSG)) {}

        ASLMessage(const ASLMessage&) = delete;
        ASLMessage& operator=(const ASLMessage&) = delete;

        ~ASLMessage() { asl_free(message_); }

        aslmsg get() const { return message_; }

       private:
        aslmsg message_;
      } asl_message;

      // By default, messages are only readable by the admin group. Explicitly
      // make them readable by the user generating the messages.
      char euid_string[12];
      snprintf(euid_string, base::size(euid_string), "%d", geteuid());
      asl_set(asl_message.get(), ASL_KEY_READ_UID, euid_string);

      // Map Chrome log severities to ASL log levels.
      const char* const asl_level_string = [](LogSeverity severity) {
#define ASL_LEVEL_STR(level) ASL_LEVEL_STR_X(level)
#define ASL_LEVEL_STR_X(level) #level
        switch (severity) {
          case LOG_INFO:
            return ASL_LEVEL_STR(ASL_LEVEL_INFO);
          case LOG_WARNING:
            return ASL_LEVEL_STR(ASL_LEVEL_WARNING);
          case LOG_ERROR:
            return ASL_LEVEL_STR(ASL_LEVEL_ERR);
          case LOG_FATAL:
            return ASL_LEVEL_STR(ASL_LEVEL_CRIT);
          default:
            return severity < 0 ? ASL_LEVEL_STR(ASL_LEVEL_DEBUG)
                                : ASL_LEVEL_STR(ASL_LEVEL_NOTICE);
        }
#undef ASL_LEVEL_STR
#undef ASL_LEVEL_STR_X
      }(severity_);
      asl_set(asl_message.get(), ASL_KEY_LEVEL, asl_level_string);

      asl_set(asl_message.get(), ASL_KEY_MSG, str_newline.c_str());

      asl_send(asl_client.get(), asl_message.get());
#else
      // Use Unified Logging (os_log) when this will only run on 10.12 and
      // later. ASL is deprecated in 10.12.

      const class OSLog {
       public:
        explicit OSLog(const char* subsystem)
            : os_log_(subsystem ? os_log_create(subsystem, "chromium_logging")
                                : OS_LOG_DEFAULT) {}

        OSLog(const OSLog&) = delete;
        OSLog& operator=(const OSLog&) = delete;

        ~OSLog() {
          if (os_log_ != OS_LOG_DEFAULT) {
            os_release(os_log_);
          }
        }

        os_log_t get() const { return os_log_; }

       private:
        os_log_t os_log_;
      } log(main_bundle_id);

      const os_log_type_t os_log_type = [](LogSeverity severity) {
        switch (severity) {
          case LOG_INFO:
            return OS_LOG_TYPE_INFO;
          case LOG_WARNING:
            return OS_LOG_TYPE_DEFAULT;
          case LOG_ERROR:
            return OS_LOG_TYPE_ERROR;
          case LOG_FATAL:
            return OS_LOG_TYPE_FAULT;
          default:
            return severity < 0 ? OS_LOG_TYPE_DEBUG : OS_LOG_TYPE_DEFAULT;
        }
      }(severity_);

      os_log_with_type(
          log.get(), os_log_type, "%{public}s", str_newline.c_str());
#endif
    }
#elif defined(OS_WIN)
    OutputDebugString(base::UTF8ToWide(str_newline).c_str());
#elif defined(OS_ANDROID)
    android_LogPriority priority =
        (severity_ < 0) ? ANDROID_LOG_VERBOSE : ANDROID_LOG_UNKNOWN;
    switch (severity_) {
      case LOG_INFO:
        priority = ANDROID_LOG_INFO;
        break;
      case LOG_WARNING:
        priority = ANDROID_LOG_WARN;
        break;
      case LOG_ERROR:
        priority = ANDROID_LOG_ERROR;
        break;
      case LOG_FATAL:
        priority = ANDROID_LOG_FATAL;
        break;
    }
    // The Android system may truncate the string if it's too long.
    __android_log_write(priority, "chromium", str_newline.c_str());
#elif defined(OS_FUCHSIA)
  fx_log_severity_t fx_severity;
  switch (severity_) {
    case LOG_INFO:
      fx_severity = FX_LOG_INFO;
      break;
    case LOG_WARNING:
      fx_severity = FX_LOG_WARNING;
      break;
    case LOG_ERROR:
      fx_severity = FX_LOG_ERROR;
      break;
    case LOG_FATAL:
      fx_severity = FX_LOG_FATAL;
      break;
    default:
      fx_severity = FX_LOG_INFO;
      break;
  }
  // Temporarily remove the trailing newline from |str_newline|'s C-string
  // representation, since fx_logger will add a newline of its own.
  str_newline.pop_back();
  // Ideally the tag would be the same as the caller, but this is not supported
  // right now.
  fx_logger_log_with_source(fx_log_get_logger(), fx_severity, /*tag=*/nullptr,
                            file_path_, line_,
                            str_newline.c_str() + message_start_);
  str_newline.push_back('\n');
#endif  // OS_*
  }

  if (severity_ == LOG_FATAL) {
#if defined(COMPILER_MSVC)
    __debugbreak();
#if defined(ARCH_CPU_X86_FAMILY)
    __ud2();
#elif defined(ARCH_CPU_ARM64)
    __hlt(0);
#else
#error Unsupported Windows Arch
#endif
#elif defined(ARCH_CPU_X86_FAMILY)
    asm("int3; ud2;");
#elif defined(ARCH_CPU_ARMEL)
    asm("bkpt #0; udf #0;");
#elif defined(ARCH_CPU_ARM64)
    asm("brk #0; hlt #0;");
#else
    __builtin_trap();
#endif
  }
}

void LogMessage::Init(const char* function) {
  std::string file_name(file_path_);
#if defined(OS_WIN)
  size_t last_slash = file_name.find_last_of("\\/");
#else
  size_t last_slash = file_name.find_last_of('/');
#endif
  if (last_slash != std::string::npos) {
    file_name.assign(file_name.substr(last_slash + 1));
  }

#if defined(OS_POSIX) && !defined(OS_FUCHSIA)
  pid_t pid = getpid();
#elif defined(OS_WIN)
  DWORD pid = GetCurrentProcessId();
#endif

#if defined(OS_APPLE)
  uint64_t thread;
  pthread_threadid_np(pthread_self(), &thread);
#elif defined(OS_ANDROID)
  pid_t thread = gettid();
#elif defined(OS_LINUX)
  pid_t thread = static_cast<pid_t>(syscall(__NR_gettid));
#elif defined(OS_WIN)
  DWORD thread = GetCurrentThreadId();
#endif

  // On Fuchsia, the platform is responsible for adding the process id and
  // thread id, not the process itself.
#if !defined(OS_FUCHSIA)
  stream_ << '['
          << pid
          << ':'
          << thread
          << ':'
          << std::setfill('0');
#endif

  // On Fuchsia, the platform is responsible for adding the log timestamp,
  // not the process itself.
#if defined(OS_POSIX) && !defined(OS_FUCHSIA)
  timeval tv;
  gettimeofday(&tv, nullptr);
  tm local_time;
  localtime_r(&tv.tv_sec, &local_time);
  stream_ << std::setw(4) << local_time.tm_year + 1900
          << std::setw(2) << local_time.tm_mon + 1
          << std::setw(2) << local_time.tm_mday
          << ','
          << std::setw(2) << local_time.tm_hour
          << std::setw(2) << local_time.tm_min
          << std::setw(2) << local_time.tm_sec
          << '.'
          << std::setw(6) << tv.tv_usec
          << ':';
#elif defined(OS_WIN)
  SYSTEMTIME local_time;
  GetLocalTime(&local_time);
  stream_ << std::setw(4) << local_time.wYear
          << std::setw(2) << local_time.wMonth
          << std::setw(2) << local_time.wDay
          << ','
          << std::setw(2) << local_time.wHour
          << std::setw(2) << local_time.wMinute
          << std::setw(2) << local_time.wSecond
          << '.'
          << std::setw(3) << local_time.wMilliseconds
          << ':';
#endif

  // On Fuchsia, ~LogMessage() will add the severity, filename and line
  // number when LOG_TO_SYSTEM_DEBUG_LOG is enabled, but not on
  // LOG_TO_STDERR so if LOG_TO_STDERR is enabled, print them here with
  // potentially repetition if LOG_TO_SYSTEM_DEBUG_LOG is also enabled.
#if defined(OS_FUCHSIA)
  if ((g_logging_destination & LOG_TO_STDERR)) {
#endif
    if (severity_ >= 0) {
      stream_ << log_severity_names[severity_];
    } else {
      stream_ << "VERBOSE" << -severity_;
    }

    stream_ << ' '
            << file_name
            << ':'
            << line_
            << "] ";
#if defined(OS_FUCHSIA)
  }
#endif

  message_start_ = stream_.str().size();
}

#if defined(OS_WIN)

unsigned long GetLastSystemErrorCode() {
  return GetLastError();
}

Win32ErrorLogMessage::Win32ErrorLogMessage(const char* function,
                                           const char* file_path,
                                           int line,
                                           LogSeverity severity,
                                           unsigned long err)
    : LogMessage(function, file_path, line, severity), err_(err) {
}

Win32ErrorLogMessage::~Win32ErrorLogMessage() {
  stream() << ": " << SystemErrorCodeToString(err_);
}

#elif defined(OS_POSIX)

ErrnoLogMessage::ErrnoLogMessage(const char* function,
                                 const char* file_path,
                                 int line,
                                 LogSeverity severity,
                                 int err)
    : LogMessage(function, file_path, line, severity),
      err_(err) {
}

ErrnoLogMessage::~ErrnoLogMessage() {
  stream() << ": "
           << base::safe_strerror(err_)
           << " ("
           << err_
           << ")";
}

#endif  // OS_POSIX

}  // namespace logging

std::ostream& std::operator<<(std::ostream& out, const std::u16string& str) {
  return out << base::UTF16ToUTF8(str);
}
