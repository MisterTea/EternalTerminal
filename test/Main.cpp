#include "Headers.hpp"

#include "LogHandler.hpp"

#include "gtest/gtest.h"

DEFINE_int32(v, 0, "verbose level");

int main(int argc, char **argv) {
  srand(1);
  testing::InitGoogleTest(&argc, argv);

  // Setup easylogging configurations
  el::Configurations defaultConf =
      et::LogHandler::SetupLogHandler(&argc, &argv);
  defaultConf.setGlobally(el::ConfigurationType::ToFile, "false");
  el::Loggers::setVerboseLevel(FLAGS_v);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  return RUN_ALL_TESTS();
}
