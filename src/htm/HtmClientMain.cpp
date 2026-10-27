#include <cxxopts.hpp>

#include "ControlMode.hpp"
#include "DaemonCreator.hpp"
#include "HtmClient.hpp"
#include "HtmClientPlatform.hpp"
#include "HtmServer.hpp"
#include "IpcPairClient.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "PipeSocketHandler.hpp"
#include "PseudoTerminalConsole.hpp"
#include "RawSocketUtils.hpp"
#include "SubprocessUtils.hpp"
#include "WinsockContext.hpp"

using namespace et;

namespace {
unique_ptr<PseudoTerminalConsole> gConsole;

void restoreTerminal() {
  if (gConsole) {
    gConsole->teardown();
  }
}
}  // namespace

int main(int argc, char** argv) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);
  WinsockContext winsockContext;
  {
    std::error_code cwdError;
    const fs::path cwd = fs::current_path(cwdError);
    if (!cwdError) {
      setenv("HTM_INITIAL_CWD", cwd.string().c_str(), 1);
    }
  }
  // Parse command line arguments
  cxxopts::Options options("htm", "Headless terminal multiplexer");
  options.allow_unrecognised_options();

  options.add_options()       //
      ("help", "Print help")  //
      ("x,kill-other-sessions",
       "kill all old sessions belonging to the user")  //
      ;

  auto result = options.parse(argc, argv);
  if (result.count("help")) {
    CLOG(INFO, "stdout") << options.help({}) << endl;
    exit(0);
  }

  setvbuf(stdin, NULL, _IONBF, 0);   // turn off buffering
  setvbuf(stdout, NULL, _IONBF, 0);  // turn off buffering

  gConsole.reset(new PseudoTerminalConsole());
  gConsole->setup();

  htm_client_platform::installTerminationHandlers(restoreTerminal);

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  el::Loggers::setVerboseLevel(3);
  LogHandler::setupLogFiles(&defaultConf, GetTempDirectory(), "htm", false,
                            true);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  et::HandleTerminate();

  // Override easylogging handler for sigint
  ::signal(SIGINT, et::InterruptSignalHandler);

  htm_client_platform::ensureDaemon(result.count("x") > 0);

  shared_ptr<SocketHandler> socketHandler(new PipeSocketHandler());
  SocketEndpoint pipeEndpoint;
  pipeEndpoint.set_name(HtmServer::getPipeName());
  try {
    auto* htmClient = new HtmClient(socketHandler, pipeEndpoint);
    htmClient->run();
  } catch (const std::exception& ex) {
    LOG(ERROR) << "htm client exiting: " << ex.what();
    return htm_client_platform::finishClient(1, restoreTerminal);
  }

  return htm_client_platform::finishClient(0, restoreTerminal);
}
