// Copyright 2006-2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINI_CHROMIUM_BASE_LOGGING_H_
#define MINI_CHROMIUM_BASE_LOGGING_H_

#include <errno.h>
#include <stdint.h>

#include <limits>
#include <sstream>
#include <string>

#include "base/files/file_path.h"
#include "build/build_config.h"

namespace logging {

// A bitmask of potential logging destinations.
using LoggingDestination = uint32_t;

// Specifies where logs will be written. Multiple destinations can be specified
// with bitwise OR.
// Unless destination is LOG_NONE, all logs with severity ERROR and above will
// be written to stderr in addition to the specified destination.
enum : LoggingDestination {
  LOG_NONE = 0,
  LOG_TO_FILE = 1 << 0,
  LOG_TO_SYSTEM_DEBUG_LOG = 1 << 1,
  LOG_TO_STDERR = 1 << 2,

  LOG_TO_ALL = LOG_TO_FILE | LOG_TO_SYSTEM_DEBUG_LOG | LOG_TO_STDERR,

#if BUILDFLAG(IS_WIN)
  LOG_DEFAULT = LOG_TO_FILE,
#elif BUILDFLAG(IS_FUCHSIA)
  LOG_DEFAULT = LOG_TO_SYSTEM_DEBUG_LOG,
#elif BUILDFLAG(IS_POSIX)
  LOG_DEFAULT = LOG_TO_SYSTEM_DEBUG_LOG | LOG_TO_STDERR,
#endif
};

typedef int LogSeverity;
const LogSeverity LOG_VERBOSE = -1;
const LogSeverity LOG_INFO = 0;
const LogSeverity LOG_WARNING = 1;
const LogSeverity LOG_ERROR = 2;
const LogSeverity LOG_ERROR_REPORT = 3;
const LogSeverity LOG_FATAL = 4;
const LogSeverity LOG_NUM_SEVERITIES = 5;

struct LoggingSettings {
  LoggingDestination logging_dest = LOG_DEFAULT;

  // The log file path, used when logging_dest includes LOG_TO_FILE. The file
  // is opened lazily for appending on the first emitted message.
  base::FilePath log_file_path;

  // Minimum severity that will be emitted. Defaults to LOG_INFO so every
  // non-verbose message passes through.
  int min_log_level = LOG_INFO;
};

// Sets the logging destination.
bool InitLogging(const LoggingSettings& settings);

#if defined(NDEBUG)
const LogSeverity LOG_DFATAL = LOG_ERROR;
#else
const LogSeverity LOG_DFATAL = LOG_FATAL;
#endif

typedef bool (*LogMessageHandlerFunction)(LogSeverity severity,
                                          const char* file_poath,
                                          int line,
                                          size_t message_start,
                                          const std::string& string);

void SetLogMessageHandler(LogMessageHandlerFunction log_message_handler);
LogMessageHandlerFunction GetLogMessageHandler();

int GetMinLogLevel();
void SetMinLogLevel(int level);

static inline int GetVlogLevel(const char*) {
  return std::numeric_limits<int>::max();
}

#if BUILDFLAG(IS_WIN)
// This is just ::GetLastError, but out-of-line to avoid including windows.h in
// such a widely used place.
unsigned long GetLastSystemErrorCode();
std::string SystemErrorCodeToString(unsigned long error_code);
#elif BUILDFLAG(IS_POSIX)
static inline int GetLastSystemErrorCode() {
  return errno;
}
#endif

class LogMessage {
 public:
  LogMessage(const char* function,
             const char* file_path,
             int line,
             LogSeverity severity);
  LogMessage(const char* function,
             const char* file_path,
             int line,
             std::string* result);

  LogMessage(const LogMessage&) = delete;
  LogMessage& operator=(const LogMessage&) = delete;

  virtual ~LogMessage();

  std::ostream& stream() { return stream_; }

 protected:
  void Flush();

 private:
  void Init(const char* function);

  std::ostringstream stream_;
  const char* file_path_;
  size_t message_start_;
  const int line_;
  LogSeverity severity_;
};

class LogMessageFatal final : public LogMessage {
 public:
  using LogMessage::LogMessage;
  [[noreturn]] ~LogMessageFatal() override;
};

#if BUILDFLAG(IS_WIN)
class Win32ErrorLogMessage : public LogMessage {
 public:
  Win32ErrorLogMessage(const char* function,
                       const char* file_path,
                       int line,
                       LogSeverity severity,
                       unsigned long err);

  Win32ErrorLogMessage(const Win32ErrorLogMessage&) = delete;
  Win32ErrorLogMessage& operator=(const Win32ErrorLogMessage&) = delete;

  ~Win32ErrorLogMessage();

 protected:
  void AppendError();

 private:
  unsigned long err_;
};

class Win32ErrorLogMessageFatal final : public Win32ErrorLogMessage {
 public:
  using Win32ErrorLogMessage::Win32ErrorLogMessage;
  [[noreturn]] ~Win32ErrorLogMessageFatal() override;
};

#elif BUILDFLAG(IS_POSIX)
class ErrnoLogMessage : public LogMessage {
 public:
  ErrnoLogMessage(const char* function,
                  const char* file_path,
                  int line,
                  LogSeverity severity,
                  int err);

  ErrnoLogMessage(const ErrnoLogMessage&) = delete;
  ErrnoLogMessage& operator=(const ErrnoLogMessage&) = delete;

  ~ErrnoLogMessage();

 protected:
  void AppendError();

 private:
  int err_;
};

class ErrnoLogMessageFatal final : public ErrnoLogMessage {
 public:
  using ErrnoLogMessage::ErrnoLogMessage;
  [[noreturn]] ~ErrnoLogMessageFatal() override;
};
#endif

}  // namespace logging

#if defined(COMPILER_MSVC)
#define FUNCTION_SIGNATURE __FUNCSIG__
#else
#define FUNCTION_SIGNATURE __PRETTY_FUNCTION__
#endif

#define COMPACT_GOOGLE_LOG_EX_INFO(ClassName, ...) \
    logging::ClassName(FUNCTION_SIGNATURE, __FILE__, __LINE__, \
                       logging::LOG_INFO, ## __VA_ARGS__)
#define COMPACT_GOOGLE_LOG_EX_WARNING(ClassName, ...) \
    logging::ClassName(FUNCTION_SIGNATURE, __FILE__, __LINE__, \
                       logging::LOG_WARNING, ## __VA_ARGS__)
#define COMPACT_GOOGLE_LOG_EX_ERROR(ClassName, ...) \
    logging::ClassName(FUNCTION_SIGNATURE, __FILE__, __LINE__, \
                       logging::LOG_ERROR, ## __VA_ARGS__)
#define COMPACT_GOOGLE_LOG_EX_ERROR_REPORT(ClassName, ...) \
    logging::ClassName(FUNCTION_SIGNATURE, __FILE__, __LINE__, \
                       logging::LOG_ERROR_REPORT, ## __VA_ARGS__)
#define COMPACT_GOOGLE_LOG_EX_FATAL(ClassName, ...) \
  logging::ClassName##Fatal(FUNCTION_SIGNATURE,     \
                            __FILE__,               \
                            __LINE__,               \
                            logging::LOG_FATAL,     \
                            ##__VA_ARGS__)
#define COMPACT_GOOGLE_LOG_EX_DFATAL(ClassName, ...) \
    logging::ClassName(FUNCTION_SIGNATURE, __FILE__, __LINE__, \
                       logging::LOG_DFATAL, ## __VA_ARGS__)

#define COMPACT_GOOGLE_LOG_INFO \
    COMPACT_GOOGLE_LOG_EX_INFO(LogMessage)
#define COMPACT_GOOGLE_LOG_WARNING \
    COMPACT_GOOGLE_LOG_EX_WARNING(LogMessage)
#define COMPACT_GOOGLE_LOG_ERROR \
    COMPACT_GOOGLE_LOG_EX_ERROR(LogMessage)
#define COMPACT_GOOGLE_LOG_ERROR_REPORT \
    COMPACT_GOOGLE_LOG_EX_ERROR_REPORT(LogMessage)
// TODO(crbug.com/40254046): Make LOG(FATAL) understood as [[noreturn]]. See
// Chromium or absl implementations for LogMessageFatal subclasses where the
// destructor is annotated as [[noreturn]].
#define COMPACT_GOOGLE_LOG_FATAL \
    COMPACT_GOOGLE_LOG_EX_FATAL(LogMessage)
#define COMPACT_GOOGLE_LOG_DFATAL \
    COMPACT_GOOGLE_LOG_EX_DFATAL(LogMessage)

#if BUILDFLAG(IS_WIN)

// wingdi.h defines ERROR 0. We don't want to include windows.h here, and we
// want to allow "LOG(ERROR)", which will expand to LOG_0.

// This will not cause a warning if the RHS text is identical to that in
// wingdi.h (which it is).
#define ERROR 0

#define COMPACT_GOOGLE_LOG_EX_0(ClassName, ...) \
  COMPACT_GOOGLE_LOG_EX_ERROR(ClassName , ##__VA_ARGS__)
#define COMPACT_GOOGLE_LOG_0 COMPACT_GOOGLE_LOG_ERROR
namespace logging {
const LogSeverity LOG_0 = LOG_ERROR;
}  // namespace logging

#endif  // BUILDFLAG(IS_WIN)

#define LAZY_STREAM(stream, condition) \
  switch (0)                           \
  case 0:                              \
  default:                             \
    if (!(condition))                  \
      ;                                \
    else                               \
      (stream)

// FATAL is always enabled and required to be resolved in compile time for
// LOG(FATAL) to be properly understood as [[noreturn]].
#define LOG_IS_ON(severity)                               \
  ((::logging::LOG_##severity) == ::logging::LOG_FATAL || \
   (::logging::LOG_##severity) >= ::logging::GetMinLogLevel())
#define VLOG_IS_ON(verbose_level) \
    ((verbose_level) <= ::logging::GetVlogLevel(__FILE__))

#define LOG_STREAM(severity) COMPACT_GOOGLE_LOG_ ## severity.stream()
#define VLOG_STREAM(verbose_level) \
    logging::LogMessage(FUNCTION_SIGNATURE, __FILE__, __LINE__, \
                        -verbose_level).stream()

#if BUILDFLAG(IS_WIN)
#define PLOG_STREAM(severity) COMPACT_GOOGLE_LOG_EX_ ## severity( \
    Win32ErrorLogMessage, ::logging::GetLastSystemErrorCode()).stream()
#define VPLOG_STREAM(verbose_level)                                       \
    logging::Win32ErrorLogMessage(FUNCTION_SIGNATURE, __FILE__, __LINE__, \
                                  -verbose_level,                         \
                                  ::logging::GetLastSystemErrorCode()).stream()
#elif BUILDFLAG(IS_POSIX)
#define PLOG_STREAM(severity) COMPACT_GOOGLE_LOG_EX_ ## severity( \
    ErrnoLogMessage, ::logging::GetLastSystemErrorCode()).stream()
#define VPLOG_STREAM(verbose_level) \
    logging::ErrnoLogMessage(FUNCTION_SIGNATURE, __FILE__, __LINE__, \
                             -verbose_level, \
                             ::logging::GetLastSystemErrorCode()).stream()
#endif

#define LOG(severity) LAZY_STREAM(LOG_STREAM(severity), LOG_IS_ON(severity))
#define LOG_IF(severity, condition) \
    LAZY_STREAM(LOG_STREAM(severity), LOG_IS_ON(severity) && (condition))
#define LOG_ASSERT(condition) \
    LOG_IF(FATAL, !(condition)) << "Assertion failed: " # condition ". "

#define VLOG(verbose_level) \
    LAZY_STREAM(VLOG_STREAM(verbose_level), VLOG_IS_ON(verbose_level))
#define VLOG_IF(verbose_level, condition) \
    LAZY_STREAM(VLOG_STREAM(verbose_level), \
                VLOG_IS_ON(verbose_level) && (condition))

#define PLOG(severity) LAZY_STREAM(PLOG_STREAM(severity), LOG_IS_ON(severity))
#define PLOG_IF(severity, condition) \
    LAZY_STREAM(PLOG_STREAM(severity), LOG_IS_ON(severity) && (condition))

#define VPLOG(verbose_level) \
    LAZY_STREAM(VPLOG_STREAM(verbose_level), VLOG_IS_ON(verbose_level))
#define VPLOG_IF(verbose_level, condition) \
    LAZY_STREAM(VPLOG_STREAM(verbose_level), \
                VLOG_IS_ON(verbose_level) && (condition))

#if defined(NDEBUG)
#define DLOG_IS_ON(severity) 0
#define DVLOG_IS_ON(verbose_level) 0
#define DCHECK_IS_ON() 0
#else
#define DLOG_IS_ON(severity) LOG_IS_ON(severity)
#define DVLOG_IS_ON(verbose_level) VLOG_IS_ON(verbose_level)
#define DCHECK_IS_ON() 1
#endif

#define DLOG(severity) LAZY_STREAM(LOG_STREAM(severity), DLOG_IS_ON(severity))
#define DLOG_IF(severity, condition) \
    LAZY_STREAM(LOG_STREAM(severity), DLOG_IS_ON(severity) && (condition))
#define DLOG_ASSERT(condition) \
    DLOG_IF(FATAL, !(condition)) << "Assertion failed: " # condition ". "

#define DVLOG(verbose_level) \
    LAZY_STREAM(VLOG_STREAM(verbose_level), DVLOG_IS_ON(verbose_level))
#define DVLOG_IF(verbose_level, condition) \
    LAZY_STREAM(VLOG_STREAM(verbose_level), \
                DVLOG_IS_ON(verbose_level) && (condition))

#define DPLOG(severity) LAZY_STREAM(PLOG_STREAM(severity), DLOG_IS_ON(severity))
#define DPLOG_IF(severity, condition) \
    LAZY_STREAM(PLOG_STREAM(severity), DLOG_IS_ON(severity) && (condition))

#define DVPLOG(verbose_level) \
    LAZY_STREAM(VPLOG_STREAM(verbose_level), DVLOG_IS_ON(verbose_level))
#define DVPLOG_IF(verbose_level, condition) \
    LAZY_STREAM(VPLOG_STREAM(verbose_level), \
                DVLOG_IS_ON(verbose_level) && (condition))

namespace std {
ostream& operator<<(ostream& out, const u16string& str);
}  // namespace std

#endif  // MINI_CHROMIUM_BASE_LOGGING_H_
