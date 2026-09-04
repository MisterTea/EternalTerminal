#include "HtmServer.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "PipeSocketHandler.hpp"
#include "WinsockContext.hpp"

#ifndef WIN32
#include <signal.h>
#endif

using namespace et;

namespace {
HtmServer* gHtmServer = nullptr;

void stopHtmServer(int) {
  if (gHtmServer) {
    gHtmServer->requestStop();
  }
}

#ifndef WIN32
void dumpHtmPanes(int) {
  if (gHtmServer) {
    gHtmServer->requestPaneDump();
  }
}
#endif
}  // namespace

int main(int argc, char** argv) {
  // Version string need to be set before GFLAGS parse arguments
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);
#ifdef WIN32
  WinsockContext winsockContext;
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
  ::signal(SIGUSR1, dumpHtmPanes);
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
  HANDLE paneDumpEvent = CreateEventA(
      NULL, TRUE, FALSE, HtmServer::getPaneDumpEventName().c_str());
  if (!paneDumpEvent) {
    CloseHandle(shutdownEvent);
    STFATAL << "Failed to create HTM pane-dump event: " << GetLastError();
  }
  // A previous daemon may have signaled the named event while this process was
  // starting. This instance owns the event now, so begin in the nonsignaled
  // state before publishing its listening socket.
  ResetEvent(shutdownEvent);
  ResetEvent(paneDumpEvent);
  thread eventWatcher([&]() {
    HANDLE events[2] = {paneDumpEvent, shutdownEvent};
    while (true) {
      const DWORD which = WaitForMultipleObjects(2, events, FALSE, INFINITE);
      if (which == WAIT_OBJECT_0) {
        ResetEvent(paneDumpEvent);
        htm.requestPaneDump();
        continue;
      }
      htm.requestStop();
      return;
    }
  });
#endif
  htm.run();
#ifdef WIN32
  SetEvent(shutdownEvent);
  eventWatcher.join();
  CloseHandle(paneDumpEvent);
  CloseHandle(shutdownEvent);
#endif
  gHtmServer = nullptr;
  LOG(INFO) << "Server is shutting down";

  return 0;
}
