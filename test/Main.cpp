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
  // el::Loggers::setVerboseLevel(9);

  et::HandleTerminate();

  string logDirectoryPattern = GetTempDirectory() + string("et_test_XXXXXXXX");
  string logDirectory = string(mkdtemp(&logDirectoryPattern[0]));
  CLOG(INFO, "stdout") << "Writing log to " << logDirectory << endl;
  et::LogHandler::setupLogFiles(&defaultConf, logDirectory, "log", true, true);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  TelemetryService::create(false, "", "");

  int result = Catch::Session().run(argc, argv);

  TelemetryService::get()->shutdown();
  TelemetryService::destroy();

  FATAL_FAIL(fs::remove_all(logDirectory.c_str()));
  return result;
}
