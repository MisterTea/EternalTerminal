#include "HtmServer.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "PipeSocketHandler.hpp"
#include "WinsockContext.hpp"

using namespace et;

namespace {
HtmServer* gHtmServer = nullptr;

void stopHtmServer(int) {
  if (gHtmServer) {
    gHtmServer->requestStop();
  }
}
}  // namespace

int main(int argc, char** argv) {
  // Version string need to be set before GFLAGS parse arguments
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);
#ifdef WIN32
  WinsockContext winsockContext;
  if (!SetCurrentDirectoryA(GetTempDirectory().c_str())) {
    STFATAL << "Failed to use the temp directory for HTM IPC: "
            << GetLastError();
  }
#endif

  // Setup easylogging configurations
  el::Configurations defaultConf =
      et::LogHandler::setupLogHandler(&argc, &argv);
  el::Loggers::setVerboseLevel(3);
  LogHandler::setupLogFiles(&defaultConf, GetTempDirectory(), "htmd", false,
                            true);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  et::HandleTerminate();

#ifndef WIN32
  ::signal(SIGINT, stopHtmServer);
  ::signal(SIGTERM, stopHtmServer);
#else
  ::signal(SIGINT, et::InterruptSignalHandler);
#endif

  shared_ptr<SocketHandler> socketHandler(new PipeSocketHandler());
  SocketEndpoint endpoint;
  endpoint.set_name(HtmServer::getPipeName());
  HtmServer htm(socketHandler, endpoint);
  gHtmServer = &htm;
#ifdef WIN32
  HANDLE shutdownEvent = CreateEventA(
      NULL, TRUE, FALSE, HtmServer::getShutdownEventName().c_str());
  if (!shutdownEvent) {
    STFATAL << "Failed to create HTM shutdown event: " << GetLastError();
  }
  // A previous daemon may have signaled the named event while this process was
  // starting. This instance owns the event now, so begin in the nonsignaled
  // state before publishing its listening socket.
  ResetEvent(shutdownEvent);
  thread shutdownWatcher([&]() {
    WaitForSingleObject(shutdownEvent, INFINITE);
    htm.requestStop();
  });
#endif
  htm.run();
#ifdef WIN32
  SetEvent(shutdownEvent);
  shutdownWatcher.join();
  CloseHandle(shutdownEvent);
#endif
  gHtmServer = nullptr;
  LOG(INFO) << "Server is shutting down";

  return 0;
}
