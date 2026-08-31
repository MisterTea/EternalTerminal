#include <atomic>
#include <thread>

#include "HtmClient.hpp"
#include "HtmServer.hpp"
#include "HtmTestHelpers.hpp"
#include "IpcPairClient.hpp"
#include "JsonLib.hpp"
#include "TestHeaders.hpp"
#include "UserSocketOps.hpp"

#ifndef WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace et;
using namespace et::htmtest;

#ifndef WIN32
namespace {
class HtmServerHarness {
 public:
  HtmServerHarness()
      : handler(std::make_shared<PipeSocketHandler>()),
        endpoint(endpointFor(ipc.path)),
        server(handler, endpoint) {
    runner = std::thread([this]() { server.run(); });
    client.reset(new IpcPairClient(handler, endpoint));
    REQUIRE(waitUntil([&]() { return server.getEndpointFd() >= 0; }, 5000));
  }

  ~HtmServerHarness() { stop(); }

  void stop() {
    server.requestStop();
    if (client && client->getEndpointFd() >= 0) {
      try {
        sendDebugKeys(handler, client->getEndpointFd(), "x");
      } catch (...) {
      }
    }
    if (runner.joinable()) {
      runner.join();
    }
  }

  json waitForInit(int timeoutMs = 8000) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
      incoming += readUntil(handler, client->getEndpointFd(), 1, 200);
      consumeInitSequence(&incoming);
      HtmPacket packet;
      string work = incoming;
      while (popPacket(&work, &packet)) {
        if (packet.header == INIT_STATE) {
          incoming = work;
          return json::parse(packet.payload);
        }
      }
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      if (elapsed > timeoutMs) {
        FAIL("Timed out waiting for INIT_STATE");
      }
    }
  }

  bool waitForHeader(char header, int timeoutMs = 8000) {
    return waitUntil(
        [&]() {
          incoming += readUntil(handler, client->getEndpointFd(), 1, 50);
          consumeInitSequence(&incoming);
          string work = incoming;
          HtmPacket packet;
          while (popPacket(&work, &packet)) {
            if (packet.header == header) {
              incoming = work;
              lastPacket = packet;
              return true;
            }
          }
          return false;
        },
        timeoutMs);
  }

  UniqueIpcPath ipc;
  shared_ptr<PipeSocketHandler> handler;
  SocketEndpoint endpoint;
  HtmServer server;
  std::thread runner;
  unique_ptr<IpcPairClient> client;
  string incoming;
  HtmPacket lastPacket;
};
}  // namespace

TEST_CASE("HtmServer getPipeName includes the uid", "[Htm][HtmServer]") {
  string name = HtmServer::getPipeName();
  REQUIRE(name.find("htm.") != string::npos);
  REQUIRE(name.find(GetHtmIpcUser()) != string::npos);
  REQUIRE(name.find(".ipc") != string::npos);
}

TEST_CASE("HtmServer recovers, handles protocol commands, and shuts down",
          "[Htm][HtmServer]") {
  HtmServerHarness h;
  json state = h.waitForInit();
  REQUIRE(state["panes"].size() >= 1);
  string paneId = firstJsonKey(state["panes"]);

#ifdef WIN32
  sendInsertKeys(h.handler, h.client->getEndpointFd(), paneId,
                 "echo HTM_SRV_ECHO\r\n");
#else
  sendInsertKeys(h.handler, h.client->getEndpointFd(), paneId,
                 "printf 'HTM_SRV_ECHO\\n'\n");
#endif
  REQUIRE(h.waitForHeader(APPEND_TO_PANE));
  REQUIRE(h.lastPacket.payload.find(paneId) == 0);

  string tabId = sole::uuid4().str();
  string tabPane = sole::uuid4().str();
  sendPacket(h.handler, h.client->getEndpointFd(), NEW_TAB, tabId + tabPane);

  string splitPane = sole::uuid4().str();
  sendPacket(h.handler, h.client->getEndpointFd(), NEW_SPLIT,
             paneId + splitPane + string("1"));

  string nestedPane = sole::uuid4().str();
  sendPacket(h.handler, h.client->getEndpointFd(), NEW_SPLIT,
             splitPane + nestedPane + string("0"));

  sendDebugKeys(h.handler, h.client->getEndpointFd(), "d");

  int32_t cols = 80;
  int32_t rows = 24;
  string resizePayload = b64Int32(cols) + b64Int32(rows) + paneId;
  sendPacket(h.handler, h.client->getEndpointFd(), RESIZE_PANE, resizePayload);

  sendPacket(h.handler, h.client->getEndpointFd(), CLIENT_CLOSE_PANE,
             nestedPane);
  sendPacket(h.handler, h.client->getEndpointFd(), CLIENT_CLOSE_PANE, tabPane);

  sendDebugKeys(h.handler, h.client->getEndpointFd(), string(1, char(27)));
  REQUIRE(waitUntil([&]() { return h.server.getEndpointFd() < 0; }, 3000));

  h.incoming.clear();
  h.client.reset(new IpcPairClient(h.handler, h.endpoint));
  REQUIRE(waitUntil([&]() { return h.server.getEndpointFd() >= 0; }, 5000));
  json recovered = h.waitForInit();
  REQUIRE(recovered["panes"].size() >= 1);

  sendDebugKeys(h.handler, h.client->getEndpointFd(), "x");
  h.stop();
}

TEST_CASE("HtmServer stops when the last pane is closed", "[Htm][HtmServer]") {
  HtmServerHarness h;
  json state = h.waitForInit();
  string paneId = firstJsonKey(state["panes"]);
  sendPacket(h.handler, h.client->getEndpointFd(), CLIENT_CLOSE_PANE, paneId);
  REQUIRE(waitUntil(
      [&]() {
        return !h.runner.joinable() || h.server.getEndpointFd() < 0;
      },
      5000));
  h.stop();
}

TEST_CASE("HtmServer recovers from a client disconnect error",
          "[Htm][HtmServer]") {
  HtmServerHarness h;
  h.waitForInit();
  h.client->closeEndpoint();
  REQUIRE(waitUntil([&]() { return h.server.getEndpointFd() < 0; }, 5000));
  h.stop();
}

TEST_CASE("HtmServer survives an abrupt client hangup", "[Htm][HtmServer]") {
  class DroppingClient : public IpcPairClient {
   public:
    using IpcPairClient::IpcPairClient;
    void hangup() {
      socketHandler->close(endpointFd);
      endpointFd = -1;
    }
  };

  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path);
  HtmServer server(handler, endpoint);
  std::thread runner([&]() { server.run(); });
  DroppingClient client(handler, endpoint);
  REQUIRE(waitUntil([&]() { return server.getEndpointFd() >= 0; }, 5000));
  client.hangup();
  REQUIRE(waitUntil([&]() { return server.getEndpointFd() < 0; }, 5000));
  REQUIRE(runner.joinable());
  server.requestStop();
  runner.join();
}

TEST_CASE("HtmServer disconnects on a stray non-protocol byte",
          "[Htm][HtmServer]") {
  HtmServerHarness h;
  h.waitForInit();
  char newline = '\n';
  h.handler->writeAllOrThrow(h.client->getEndpointFd(), &newline, 1, false);
  REQUIRE(waitUntil([&]() { return h.server.getEndpointFd() < 0; }, 5000));
  h.stop();
}

TEST_CASE("HtmClient forwards stdin and exits on SESSION_END",
          "[Htm][HtmClient]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path);

  class RecoveringServer : public IpcPairServer {
   public:
    RecoveringServer(shared_ptr<SocketHandler> socketHandler,
                     const SocketEndpoint& ep)
        : IpcPairServer(socketHandler, ep) {}
    void recover() override {}
  };
  RecoveringServer server(handler, endpoint);

  int inpipe[2];
  int outpipe[2];
  REQUIRE(pipe(inpipe) == 0);
  REQUIRE(pipe(outpipe) == 0);

  fflush(NULL);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    dup2(inpipe[0], STDIN_FILENO);
    dup2(outpipe[1], STDOUT_FILENO);
    close(inpipe[0]);
    close(inpipe[1]);
    close(outpipe[0]);
    close(outpipe[1]);
    try {
      auto childHandler = std::make_shared<PipeSocketHandler>();
      HtmClient client(childHandler, endpoint);
      client.run();
    } catch (...) {
    }
    UserSocketOps::coverageExit(0);
  }

  close(inpipe[0]);
  close(outpipe[1]);
  REQUIRE(waitUntil(
      [&]() {
        server.pollAccept();
        return server.getEndpointFd() >= 0;
      },
      5000));

  const char keys[] = "abc";
  REQUIRE(::write(inpipe[1], keys, 3) == 3);
  string fromClient = readUntil(handler, server.getEndpointFd(), 3, 3000);
  REQUIRE(fromClient.find("abc") != string::npos);

  const char hello[] = "xyz";
  handler->writeAllOrThrow(server.getEndpointFd(), hello, 3, false);
  REQUIRE(waitUntil(
      [&]() {
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(outpipe[0], &rfd);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        if (select(outpipe[0] + 1, &rfd, NULL, NULL, &tv) <= 0) {
          return false;
        }
        char buf[16];
        ssize_t n = ::read(outpipe[0], buf, sizeof(buf));
        return n > 0 && string(buf, size_t(n)).find("xyz") != string::npos;
      },
      3000));

  server.closeEndpoint();
  int status = 0;
  REQUIRE(waitUntil(
      [&]() { return waitpid(pid, &status, WNOHANG) == pid; }, 5000));
  close(inpipe[1]);
  close(outpipe[0]);
}

TEST_CASE("HtmClient exits when the server closes without SESSION_END",
          "[Htm][HtmClient]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  auto endpoint = endpointFor(ipc.path);

  class SilentCloseServer : public IpcPairServer {
   public:
    SilentCloseServer(shared_ptr<SocketHandler> socketHandler,
                      const SocketEndpoint& ep)
        : IpcPairServer(socketHandler, ep) {}
    void recover() override {}
    void dropClient() {
      socketHandler->close(endpointFd);
      endpointFd = -1;
    }
  };
  SilentCloseServer server(handler, endpoint);

  int inpipe[2];
  int outpipe[2];
  REQUIRE(pipe(inpipe) == 0);
  REQUIRE(pipe(outpipe) == 0);
  fflush(NULL);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    dup2(inpipe[0], STDIN_FILENO);
    dup2(outpipe[1], STDOUT_FILENO);
    close(inpipe[0]);
    close(inpipe[1]);
    close(outpipe[0]);
    close(outpipe[1]);
    try {
      auto childHandler = std::make_shared<PipeSocketHandler>();
      HtmClient client(childHandler, endpoint);
      client.run();
    } catch (...) {
    }
    UserSocketOps::coverageExit(0);
  }
  close(inpipe[0]);
  close(outpipe[1]);
  REQUIRE(waitUntil(
      [&]() {
        server.pollAccept();
        return server.getEndpointFd() >= 0;
      },
      5000));
  server.dropClient();
  int status = 0;
  REQUIRE(waitUntil(
      [&]() { return waitpid(pid, &status, WNOHANG) == pid; }, 5000));
  close(inpipe[1]);
  close(outpipe[0]);
}
#endif
