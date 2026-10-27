// Copyright 2006-2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/logging.h"

#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <iomanip>
#include <memory>
#include <ostream>

#if BUILDFLAG(IS_POSIX)
#include <paths.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "base/posix/safe_strerror.h"
#endif  // BUILDFLAG(IS_POSIX)

#if BUILDFLAG(IS_APPLE)
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <pthread.h>
#elif BUILDFLAG(IS_LINUX)
#include <sys/syscall.h>
#include <sys/types.h>
#elif BUILDFLAG(IS_WIN)
#include <windows.h>
#elif BUILDFLAG(IS_ANDROID)
#include <android/log.h>
#elif BUILDFLAG(IS_FUCHSIA)
#include <lib/syslog/cpp/log_message_impl.h>
#endif

#include "base/check_op.h"
#include "base/files/file_util.h"
#include "base/immediate_crash.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"

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

std::atomic<LoggingDestination> g_logging_destination{LOG_DEFAULT};

std::atomic<int> g_min_log_level{LOG_INFO};

// Closes a log file silently on destruction. Unlike base::ScopedFILECloser,
// LogFileCloser does not PLOG on fclose failure. We may be inside static
// destruction where a re-entrant Flush() would access the log file while it
// is being torn down.
struct LogFileCloser {
  void operator()(FILE* file) const {
    if (file) {
      fclose(file);
    }
  }
};

// File-bound logging state. All fields are protected by `lock`. The handle
// is opened lazily on the first emitted message; `enabled` is cleared when
// the open fails or the path is empty, and reset by InitLogging.
struct LogFile {
  base::Lock lock;
  base::FilePath path;
  std::unique_ptr<FILE, LogFileCloser> handle;
  bool enabled = true;
};
LogFile g_log_file;

}  // namespace

int GetMinLogLevel() {
  return g_min_log_level.load(std::memory_order_relaxed);
}

void SetMinLogLevel(int level) {
  g_min_log_level.store(level, std::memory_order_relaxed);
}

bool InitLogging(const LoggingSettings& settings) {
  // Close any previously opened file; the new path is opened lazily on the
  // first emitted message. The old handle is moved out of the critical
  // section so its destructor runs after the lock is released.
  std::unique_ptr<FILE, LogFileCloser> old_handle;
  {
    base::AutoLock lock(g_log_file.lock);
    old_handle = std::move(g_log_file.handle);
    g_log_file.path = settings.log_file_path;
    g_log_file.enabled = true;
  }
  g_logging_destination.store(settings.logging_dest, std::memory_order_relaxed);
  g_min_log_level.store(settings.min_log_level, std::memory_order_relaxed);
  return true;
}

void SetLogMessageHandler(LogMessageHandlerFunction log_message_handler) {
  g_log_message_handler = log_message_handler;
}

LogMessageHandlerFunction GetLogMessageHandler() {
  return g_log_message_handler;
}

#if BUILDFLAG(IS_WIN)
std::string SystemErrorCodeToString(unsigned long error_code) {
  wchar_t msgbuf[256];
  DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                FORMAT_MESSAGE_MAX_WIDTH_MASK;
  DWORD len = FormatMessage(flags,
                            nullptr,
                            error_code,
                            0,
                            msgbuf,
                            static_cast<DWORD>(std::size(msgbuf)),
                            nullptr);
  if (len) {
    // Most system messages end in a period and a space. Remove the space if
    // it’s there, because the following StringPrintf() includes one.
    if (len >= 1 && msgbuf[len - 1] == ' ') {
      msgbuf[len - 1] = '\0';
    }
    return base::StringPrintf("%s (%lu)",
                              base::WideToUTF8(msgbuf).c_str(), error_code);
  }
  return base::StringPrintf("Error %lu while retrieving error %lu",
                            GetLastError(),
                            error_code);
}
#endif  // BUILDFLAG(IS_WIN)

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
  Flush();
}

void LogMessage::Flush() {
  stream_ << std::endl;
  std::string str_newline(stream_.str());

  if (g_log_message_handler &&
      g_log_message_handler(
          severity_, file_path_, line_, message_start_, str_newline)) {
    return;
  }

  const LoggingDestination destination =
      g_logging_destination.load(std::memory_order_relaxed);

  if ((destination & LOG_TO_STDERR)) {
    fprintf(stderr, "%s", str_newline.c_str());
    fflush(stderr);
  }

  if ((destination & LOG_TO_FILE)) {
    base::AutoLock lock(g_log_file.lock);
    if (g_log_file.enabled && !g_log_file.handle) {
      if (g_log_file.path.empty()) {
        g_log_file.enabled = false;
      } else {
        g_log_file.handle.reset(base::OpenFile(g_log_file.path, "a"));
        if (!g_log_file.handle) {
          g_log_file.enabled = false;
        }
      }
    }
    if (FILE* f = g_log_file.handle.get()) {
      fwrite(str_newline.data(), 1, str_newline.size(), f);
      fflush(f);
    }
  }

  if ((destination & LOG_TO_SYSTEM_DEBUG_LOG) != 0) {
#if BUILDFLAG(IS_APPLE)
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
    }
#elif BUILDFLAG(IS_WIN)
    OutputDebugString(base::UTF8ToWide(str_newline).c_str());
#elif BUILDFLAG(IS_ANDROID)
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
#elif BUILDFLAG(IS_FUCHSIA)
    fuchsia_logging::LogSeverity fx_severity;
    switch (severity_) {
      case LOG_INFO:
        fx_severity = fuchsia_logging::LogSeverity::Info;
        break;
      case LOG_WARNING:
        fx_severity = fuchsia_logging::LogSeverity::Warn;
        break;
      case LOG_ERROR:
        fx_severity = fuchsia_logging::LogSeverity::Error;
        break;
      case LOG_FATAL:
        fx_severity = fuchsia_logging::LogSeverity::Fatal;
        break;
      default:
        fx_severity = fuchsia_logging::LogSeverity::Info;
        break;
    }
    // Fuchsia's logger doesn't want the trailing newline.
    std::string_view message(str_newline);
    message.remove_suffix(1);
    message.remove_prefix(message_start_);
    // Ideally the tag would be the same as the caller, but this is not
    // supported right now.
    fuchsia_logging::LogMessage(
        fx_severity, file_path_, line_, nullptr, nullptr)
            .stream()
        << message;
#endif  // BUILDFLAG(IS_*)
  }

  if (severity_ == LOG_FATAL) {
    base::ImmediateCrash();
  }
}

void LogMessage::Init(const char* function) {
  std::string file_name(file_path_);
#if BUILDFLAG(IS_WIN)
  size_t last_slash = file_name.find_last_of("\\/");
#else
  size_t last_slash = file_name.find_last_of('/');
#endif
  if (last_slash != std::string::npos) {
    file_name.assign(file_name.substr(last_slash + 1));
  }

#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_FUCHSIA)
  pid_t pid = getpid();
#elif BUILDFLAG(IS_WIN)
  DWORD pid = GetCurrentProcessId();
#endif

#if BUILDFLAG(IS_APPLE)
  uint64_t thread;
  pthread_threadid_np(pthread_self(), &thread);
#elif BUILDFLAG(IS_ANDROID)
  pid_t thread = gettid();
#elif BUILDFLAG(IS_LINUX)
  pid_t thread = static_cast<pid_t>(syscall(__NR_gettid));
#elif BUILDFLAG(IS_WIN)
  DWORD thread = GetCurrentThreadId();
#endif

  // On Fuchsia, the platform is responsible for adding the process id and
  // thread id, not the process itself.
#if !BUILDFLAG(IS_FUCHSIA)
  stream_ << '['
          << pid
          << ':'
          << thread
          << ':'
          << std::setfill('0');
#endif

  // On Fuchsia, the platform is responsible for adding the log timestamp,
  // not the process itself.
#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_FUCHSIA)
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
#elif BUILDFLAG(IS_WIN)
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
#if BUILDFLAG(IS_FUCHSIA)
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
#if BUILDFLAG(IS_FUCHSIA)
  }
#endif

  message_start_ = stream_.str().size();
}

// We intentionally don't return from these destructors. Disable MSVC's warning
// about the destructor never returning as we do so intentionally here.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4722)
#endif

LogMessageFatal::~LogMessageFatal() {
  Flush();
  base::ImmediateCrash();
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

#if BUILDFLAG(IS_WIN)

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
  AppendError();
}

void Win32ErrorLogMessage::AppendError() {
  stream() << ": " << SystemErrorCodeToString(err_);
}

// We intentionally don't return from these destructors. Disable MSVC's warning
// about the destructor never returning as we do so intentionally here.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4722)
#endif

Win32ErrorLogMessageFatal::~Win32ErrorLogMessageFatal() {
  AppendError();
  Flush();
  base::ImmediateCrash();
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

#elif BUILDFLAG(IS_POSIX)

ErrnoLogMessage::ErrnoLogMessage(const char* function,
                                 const char* file_path,
                                 int line,
                                 LogSeverity severity,
                                 int err)
    : LogMessage(function, file_path, line, severity),
      err_(err) {
}

ErrnoLogMessage::~ErrnoLogMessage() {
  AppendError();
}

void ErrnoLogMessage::AppendError() {
  stream() << ": "
           << base::safe_strerror(err_)
           << " ("
           << err_
           << ")";
}

ErrnoLogMessageFatal::~ErrnoLogMessageFatal() {
  AppendError();
  Flush();
  base::ImmediateCrash();
}

#endif  // BUILDFLAG(IS_POSIX)

}  // namespace logging

std::ostream& std::operator<<(std::ostream& out, const std::u16string& str) {
  return out << base::UTF16ToUTF8(str);
}
