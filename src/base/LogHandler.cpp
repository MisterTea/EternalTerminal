#include "LogHandler.hpp"

INITIALIZE_EASYLOGGINGPP

namespace et {
el::Configurations LogHandler::setupLogHandler(int *argc, char ***argv) {
  // easylogging parse verbose arguments, see [Application Arguments]
  // in https://github.com/muflihun/easyloggingpp/blob/master/README.md
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

void LogHandler::setupLogFile(el::Configurations *defaultConf, string filename,
                              string maxlogsize) {
  // Enable strict log file size check
  el::Loggers::addFlag(el::LoggingFlag::StrictLogFileSizeCheck);
  defaultConf->setGlobally(el::ConfigurationType::Filename, filename);
  defaultConf->setGlobally(el::ConfigurationType::ToFile, "true");
  defaultConf->setGlobally(el::ConfigurationType::MaxLogFileSize, maxlogsize);
}

void LogHandler::rolloutHandler(const char *filename, std::size_t size) {
  // SHOULD NOT LOG ANYTHING HERE BECAUSE LOG FILE IS CLOSED!
  // REMOVE OLD LOG
  remove(filename);
}

string LogHandler::stderrToFile(const string &pathPrefix) {
  time_t rawtime;
  struct tm *timeinfo;
  char buffer[80];
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  strftime(buffer, sizeof(buffer), "%Y-%m-%d_%I-%M", timeinfo);
  string current_time(buffer);
  string stderrFilename = pathPrefix + "_stderr_" + current_time;
  FILE *stderr_stream = freopen(stderrFilename.c_str(), "w", stderr);
  if (!stderr_stream) {
    STFATAL << "Invalid filename " << stderrFilename;
  }
  setvbuf(stderr_stream, NULL, _IOLBF, BUFSIZ);  // set to line buffering
  return stderrFilename;
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
}  // namespace et
