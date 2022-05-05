#ifndef __ET_FUZZABLE_TERMINAL_SERVER__
#define __ET_FUZZABLE_TERMINAL_SERVER__

#include "TerminalServer.hpp"

namespace et {
class FuzzableTerminalServer {
 public:
  FuzzableTerminalServer() {
    serverSocketHandler.reset(new PipeSocketHandler());
    pipeSocketHandler.reset(new PipeSocketHandler());

    string tmpPath = GetTempDirectory() + string("etserver_fuzzer_XXXXXXXX");
    const string pipeDirectory = string(mkdtemp(&tmpPath[0]));

    const string serverPipePath = pipeDirectory + "/pipe_server";
    SocketEndpoint serverEndpoint;
    serverEndpoint.set_name(serverPipePath);

    const string routerPath = pipeDirectory + "/pipe_router";
    routerEndpoint.set_name(routerPath);

    terminalServer.reset(new TerminalServer(serverSocketHandler, serverEndpoint,
                                            pipeSocketHandler, routerEndpoint));
    terminalServerThread = thread([this]() { terminalServer->run(); });
  }

  ~FuzzableTerminalServer() {
    terminalServer->shutdown();
    terminalServerThread.join();
  }

  std::shared_ptr<PipeSocketHandler> serverSocketHandler;
  std::shared_ptr<PipeSocketHandler> pipeSocketHandler;

  SocketEndpoint serverEndpoint;
  SocketEndpoint routerEndpoint;

  shared_ptr<TerminalServer> terminalServer;
  thread terminalServerThread;
};

}  // namespace et

#endif  // __ET_FUZZABLE_TERMINAL_SERVER__
