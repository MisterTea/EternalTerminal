#include "HtmServer.hpp"

#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "PipeSocketHandler.hpp"

using namespace et;

int main(int argc, char **argv) {
  // Version string need to be set before GFLAGS parse arguments
  SetVersionString(string(ET_VERSION));
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);

  // Setup easylogging configurations
  el::Configurations defaultConf =
      et::LogHandler::SetupLogHandler(&argc, &argv);
  defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
  el::Loggers::setVerboseLevel(3);
  // default max log file size is 20MB for etserver
  string maxlogsize = "20971520";
  LogHandler::SetupLogFile(&defaultConf, "/tmp/htmd.log", maxlogsize);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  shared_ptr<SocketHandler> socketHandler(new PipeSocketHandler());
  HtmServer htm(socketHandler, SocketEndpoint(HtmServer::getPipeName()));
  htm.run();
  LOG(INFO) << "Server is shutting down";

  return 0;
}
