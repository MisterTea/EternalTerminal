#include "LogHandler.hpp"

#ifdef WIN32
#include <io.h>
#endif

#include <climits>

INITIALIZE_EASYLOGGINGPP

namespace et {
el::Configurations LogHandler::setupLogHandler(int* argc, char*** argv) {
  // easylogging parses verbose arguments, see [Application Arguments]
  // in https://github.com/muflihun/easyloggingpp/blob/master/README.md
  // but it is non-intuitive so we explicitly set verbosity based on cxxopts
  START_EASYLOGGINGPP(*argc, *argv);

  // Easylogging configurations
  el::Configurations defaultConf;
  defaultConf.setToDefault();
  // doc says %thread_name, but %thread is the right one
  defaultConf.setGlobally(el::ConfigurationType::Format,
                          "[%level %datetime %thread %fbase:%line] %msg");
  defaultConf.setGlobally(el::ConfigurationType::Enabled, "true");
  defaultConf.setGlobally(el::ConfigurationType::SubsecondPrecision, "3");
  defaultConf.setGlobally(el::ConfigurationType::PerformanceTracking, "false");
  defaultConf.setGlobally(el::ConfigurationType::LogFlushThreshold, "1");
  defaultConf.set(el::Level::Verbose, el::ConfigurationType::Format,
                  "[%levshort%vlevel %datetime %thread %fbase:%line] %msg");
  return defaultConf;
}

void LogHandler::setupLogFiles(el::Configurations* defaultConf,
                               const string& path, const string& filenamePrefix,
                               bool logToStdout, bool redirectStderrToFile,
                               bool appendPid, string maxlogsize) {
  auto now = std::chrono::system_clock::now();
  time_t rawtime = std::chrono::system_clock::to_time_t(now);
  struct tm* timeinfo = std::localtime(&rawtime);

  char buffer[80];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", timeinfo);
  string current_time(buffer);

  auto duration = now.time_since_epoch();
  auto microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(duration) %
      std::chrono::seconds(1);
  std::stringstream ss;
  ss << std::setw(6) << std::setfill('0') << microseconds.count();
  current_time += "." + ss.str();

  string logFilename = filenamePrefix + "-" + current_time;
  string stderrFilename = filenamePrefix + "-stderr-" + current_time;
  if (appendPid) {
    string pid = std::to_string(getpid());
    logFilename.append("_" + pid);
    stderrFilename.append("_" + pid);
  }
  logFilename.append(".log");
  stderrFilename.append(".log");
  string fullFname = createLogFile(path, logFilename);

  // Enable strict log file size check
  el::Loggers::addFlag(el::LoggingFlag::StrictLogFileSizeCheck);
  defaultConf->setGlobally(el::ConfigurationType::Filename, fullFname);
  defaultConf->setGlobally(el::ConfigurationType::ToFile, "true");
  defaultConf->setGlobally(el::ConfigurationType::MaxLogFileSize, maxlogsize);

  if (logToStdout) {
    defaultConf->setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  } else {
    defaultConf->setGlobally(el::ConfigurationType::ToStandardOutput, "false");
  }

  if (redirectStderrToFile) {
    stderrToFile(path, stderrFilename);
  }
}

void LogHandler::rolloutHandler(const char* filename, std::size_t size) {
  // SHOULD NOT LOG ANYTHING HERE BECAUSE LOG FILE IS CLOSED!
  // REMOVE OLD LOG
  remove(filename);
}

void LogHandler::setupStdoutLogger() {
  el::Logger* stdoutLogger = el::Loggers::getLogger("stdout");
  // Easylogging configurations
  el::Configurations stdoutConf;
  stdoutConf.setToDefault();
  // Values are always std::string
  stdoutConf.setGlobally(el::ConfigurationType::Format, "%msg");
  stdoutConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  stdoutConf.setGlobally(el::ConfigurationType::ToFile, "false");
  el::Loggers::reconfigureLogger(stdoutLogger, stdoutConf);
}

string LogHandler::createLogFile(const string& path, const string& filename) {
  string fullFname = path + "/" + filename;
  try {
    fs::create_directories(path);
  } catch (const fs::filesystem_error& fse) {
    CLOG(ERROR, "stdout") << "Cannot create logfile directory: " << fse.what()
                          << endl;
    throw std::runtime_error("Cannot create logfile directory: " +
                             string(fse.what()));
  }
  FATAL_FAIL(::open(fullFname.c_str(), O_NOFOLLOW | O_EXCL | O_CREAT, 0600));
  return fullFname;
}

namespace {
// stderr as it was before stderrToFile freopen'd it onto the log. SSH auth
// banners are written here so a Tailscale login URL stays on the terminal.
int preservedUserStderr = -1;

int stderrNo() {
#ifdef WIN32
  return _fileno(stderr);
#else
  return ::fileno(stderr);
#endif
}

int duplicateFd(int fd) {
#ifdef WIN32
  return _dup(fd);
#else
  return ::dup(fd);
#endif
}

void closeFd(int fd) {
#ifdef WIN32
  _close(fd);
#else
  ::close(fd);
#endif
}

void setCloexec(int fd) {
#ifdef WIN32
  intptr_t osHandle = _get_osfhandle(fd);
  if (osHandle != -1) {
    SetHandleInformation(reinterpret_cast<HANDLE>(osHandle),
                         HANDLE_FLAG_INHERIT, 0);
  }
#else
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
}

void writeBestEffort(int fd, const char* data, size_t len) {
  if (fd < 0 || data == nullptr || len == 0) {
    return;
  }
  const char* cursor = data;
  size_t remaining = len;
  while (remaining > 0) {
#ifdef WIN32
    const unsigned int chunk = remaining > static_cast<size_t>(INT_MAX)
                                   ? static_cast<unsigned int>(INT_MAX)
                                   : static_cast<unsigned int>(remaining);
    const int written = _write(fd, cursor, chunk);
#else
    const ssize_t written = ::write(fd, cursor, remaining);
#endif
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if (written == 0) {
      return;
    }
    cursor += written;
    remaining -= static_cast<size_t>(written);
  }
}
}  // namespace

void LogHandler::stderrToFile(const string& path,
                              const string& stderrFilename) {
  // Keep a copy of the terminal stderr. freopen below replaces fd 2 with the
  // log file, which is why SSH banners were invisible without --logtostdout.
  if (preservedUserStderr < 0) {
    const int saved = duplicateFd(stderrNo());
    if (saved >= 0) {
      setCloexec(saved);
      preservedUserStderr = saved;
    }
  }
  string fullFname = createLogFile(path, stderrFilename);
  FILE* stderr_stream = freopen(fullFname.c_str(), "w", stderr);
  if (!stderr_stream) {
    STFATAL << "Invalid filename " << stderrFilename;
  }
  setvbuf(stderr_stream, NULL, _IOLBF, BUFSIZ);  // set to line buffering
}

bool LogHandler::forwardSubprocessStderr(const char* data, size_t len) {
  if (preservedUserStderr < 0) {
    return false;
  }
  writeBestEffort(preservedUserStderr, data, len);
  const int current = stderrNo();
  if (current != preservedUserStderr) {
    writeBestEffort(current, data, len);
  }
  return true;
}

void LogHandler::releasePreservedUserStderr() {
  if (preservedUserStderr < 0) {
    return;
  }
  closeFd(preservedUserStderr);
  preservedUserStderr = -1;
}

}  // namespace et
