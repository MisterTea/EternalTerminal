#define CATCH_CONFIG_RUNNER

#include "LogHandler.hpp"
#include "TelemetryService.hpp"
#include "TestHeaders.hpp"

using namespace et;

int main(int argc, char **argv) {
  srand(1);

  // Setup easylogging configurations
  el::Configurations defaultConf =
      et::LogHandler::setupLogHandler(&argc, &argv);
  et::LogHandler::setupStdoutLogger();
  defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  defaultConf.setGlobally(el::ConfigurationType::ToFile, "true");
  // el::Loggers::setVerboseLevel(9);

  string logDirectoryPattern = string("/tmp/et_test_XXXXXXXX");
  string logDirectory = string(mkdtemp(&logDirectoryPattern[0]));
  string logPath = string(logDirectory) + "/log";
  CLOG(INFO, "stdout") << "Writing log to " << logPath << endl;
  et::LogHandler::setupLogFile(&defaultConf, logPath);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  TelemetryService::create(false, "", "");

  int result = Catch::Session().run(argc, argv);

  TelemetryService::get()->shutdown();
  TelemetryService::destroy();

  FATAL_FAIL(::remove(logPath.c_str()));
  FATAL_FAIL(::remove(logDirectory.c_str()));
  return result;
}
