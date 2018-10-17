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
  defaultConf.setGlobally(el::ConfigurationType::ToFile, "false");
  el::Loggers::setVerboseLevel(FLAGS_v);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  if (FLAGS_stress) {
    for (int a = 0; a < 99; a++) {
      if (RUN_ALL_TESTS()) {
        LOG(FATAL) << "Tests failed";
      }
    }
  }

  return RUN_ALL_TESTS();
}
