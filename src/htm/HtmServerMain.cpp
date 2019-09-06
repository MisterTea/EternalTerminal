#include "HtmServer.hpp"

#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "PipeSocketHandler.hpp"

using namespace et;

int main(int argc, char **argv) {
  // Version string need to be set before GFLAGS parse arguments
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);

  // Setup easylogging configurations
  el::Configurations defaultConf =
      et::LogHandler::setupLogHandler(&argc, &argv);
  defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
  el::Loggers::setVerboseLevel(3);
  // default max log file size is 20MB for etserver
  string maxlogsize = "20971520";
  LogHandler::setupLogFile(&defaultConf, "/tmp/htmd.log", maxlogsize);
  // Redirect std streams to a file
  LogHandler::stderrToFile("/tmp/htmd");

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  shared_ptr<SocketHandler> socketHandler(new PipeSocketHandler());
  SocketEndpoint endpoint;
  endpoint.set_name(HtmServer::getPipeName());
  HtmServer htm(socketHandler, endpoint);
  htm.run();
  LOG(INFO) << "Server is shutting down";

  return 0;
}
