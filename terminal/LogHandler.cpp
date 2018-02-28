#include "LogHandler.hpp"

namespace et {
el::Configurations LogHandler::SetupLogHandler(int argc, char **argv) {
  // easylogging parse verbose arguments, see [Application Arguments]
  // in https://github.com/muflihun/easyloggingpp/blob/master/README.md
  START_EASYLOGGINGPP(argc, argv);
  // GFLAGS parse command line arguments
  gflags::ParseCommandLineFlags(&argc, &argv, false);

  // Easylogging configurations
  el::Configurations defaultConf;
  defaultConf.setToDefault();
  defaultConf.setGlobally(el::ConfigurationType::Format,
                          "[%level %datetime %thread %fbase:%line] %msg");
  defaultConf.setGlobally(el::ConfigurationType::Enabled, "true");
  defaultConf.setGlobally(el::ConfigurationType::MaxLogFileSize, "20971520");
  defaultConf.setGlobally(el::ConfigurationType::SubsecondPrecision, "3");
  defaultConf.setGlobally(el::ConfigurationType::PerformanceTracking, "false");
  defaultConf.setGlobally(el::ConfigurationType::LogFlushThreshold, "1");
  defaultConf.set(el::Level::Verbose, el::ConfigurationType::Format,
                  "[%levshort%vlevel %datetime %thread %fbase:%line] %msg");
  return defaultConf;
}
}  // namespace et
