#include "LogHandler.hpp"

INITIALIZE_EASYLOGGINGPP

namespace et {
el::Configurations LogHandler::setupLogHandler(int *argc, char ***argv) {
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

void LogHandler::setupLogFiles(el::Configurations *defaultConf,
                               const string &path, const string &filenamePrefix,
                               bool logToStdout, bool redirectStderrToFile,
                               bool appendPid, string maxlogsize) {
  time_t rawtime;
  struct tm *timeinfo;
  char buffer[80];
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S.%f", timeinfo);
  string current_time(buffer);
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

void LogHandler::rolloutHandler(const char *filename, std::size_t size) {
  // SHOULD NOT LOG ANYTHING HERE BECAUSE LOG FILE IS CLOSED!
  // REMOVE OLD LOG
  remove(filename);
}

void LogHandler::setupStdoutLogger() {
  el::Logger *stdoutLogger = el::Loggers::getLogger("stdout");
  // Easylogging configurations
  el::Configurations stdoutConf;
  stdoutConf.setToDefault();
  // Values are always std::string
  stdoutConf.setGlobally(el::ConfigurationType::Format, "%msg");
  stdoutConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  stdoutConf.setGlobally(el::ConfigurationType::ToFile, "false");
  el::Loggers::reconfigureLogger(stdoutLogger, stdoutConf);
}

string LogHandler::createLogFile(const string &path, const string &filename) {
  string fullFname = path + "/" + filename;
  try {
    fs::create_directories(path);
  } catch (const fs::filesystem_error &fse) {
    CLOG(ERROR, "stdout") << "Cannot create logfile directory: " << fse.what()
                          << endl;
    exit(1);
  }
#ifdef WIN32
  // O_NOFOLLOW does not exist on windows
  FATAL_FAIL(::open(fullFname.c_str(), O_EXCL | O_CREAT, 0600));
#else
  FATAL_FAIL(::open(fullFname.c_str(), O_NOFOLLOW | O_EXCL | O_CREAT, 0600));
#endif
  return fullFname;
}

void LogHandler::stderrToFile(const string &path,
                              const string &stderrFilename) {
  string fullFname = createLogFile(path, stderrFilename);
  FILE *stderr_stream = freopen(fullFname.c_str(), "w", stderr);
  if (!stderr_stream) {
    STFATAL << "Invalid filename " << stderrFilename;
  }
  setvbuf(stderr_stream, NULL, _IOLBF, BUFSIZ);  // set to line buffering
}

}  // namespace et
