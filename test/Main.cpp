#include "Headers.hpp"

#include "LogHandler.hpp"

#include "gtest/gtest.h"

DEFINE_int32(v, 0, "verbose level");
DEFINE_bool(stress, false, "Stress test");

int main(int argc, char **argv) {
  srand(1);
  testing::InitGoogleTest(&argc, argv);

  // Setup easylogging configurations
  el::Configurations defaultConf =
      et::LogHandler::setupLogHandler(&argc, &argv);
  defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  defaultConf.setGlobally(el::ConfigurationType::ToFile, "true");
  el::Loggers::setVerboseLevel(FLAGS_v);

  string stderrPathPrefix =
      string("/tmp/et_test_") + to_string(rand()) + string("_");
  string stderrPath = et::LogHandler::stderrToFile(stderrPathPrefix);
  cout << "Writing stderr to " << stderrPath << endl;

  string logDirectoryPattern = string("/tmp/et_test_XXXXXXXX");
  string logDirectory = string(mkdtemp(&logDirectoryPattern[0]));
  string logPath = string(logDirectory) + "/log";
  cout << "Writing log to " << logPath << endl;
  et::LogHandler::setupLogFile(&defaultConf, logPath);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  if (FLAGS_stress) {
    for (int a = 0; a < 99; a++) {
      if (RUN_ALL_TESTS()) {
        LOG(FATAL) << "Tests failed";
      }
    }
  }

  int retval = RUN_ALL_TESTS();

  FATAL_FAIL(::remove(stderrPath.c_str()));
  FATAL_FAIL(::remove(logPath.c_str()));
  FATAL_FAIL(::remove(logDirectory.c_str()));
  return retval;
}
