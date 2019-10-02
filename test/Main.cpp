#define CATCH_CONFIG_RUNNER

#include "TestHeaders.hpp"

#include "LogHandler.hpp"

int main(int argc, char **argv) {
  srand(1);

  // Setup easylogging configurations
  el::Configurations defaultConf =
      et::LogHandler::setupLogHandler(&argc, &argv);
  defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  defaultConf.setGlobally(el::ConfigurationType::ToFile, "true");
  // el::Loggers::setVerboseLevel(9);

  string logDirectoryPattern = string("/tmp/et_test_XXXXXXXX");
  string logDirectory = string(mkdtemp(&logDirectoryPattern[0]));
  string logPath = string(logDirectory) + "/log";
  cout << "Writing log to " << logPath << endl;
  et::LogHandler::setupLogFile(&defaultConf, logPath);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  int result = Catch::Session().run(argc, argv);

  FATAL_FAIL(::remove(logPath.c_str()));
  FATAL_FAIL(::remove(logDirectory.c_str()));
  return result;
}
