#include <atomic>
#include <cstdio>
#include <map>
#include <set>
#include <thread>
#include <utility>

#include "HtmClient.hpp"
#include "HtmServer.hpp"
#include "HtmTestHelpers.hpp"
#include "IpcPairClient.hpp"
#include "JsonLib.hpp"
#include "TestHeaders.hpp"
#include "UserSocketOps.hpp"

#ifndef WIN32
#include <errno.h>
#include <fcntl.h>
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

TEST_CASE("HtmServer streams concurrent output from tabs and splits",
          "[Htm][HtmServer][features]") {
  HtmServerHarness h;
  json state = h.waitForInit();
  string p0 = firstJsonKey(state["panes"]);

  string tab1 = sole::uuid4().str();
  string pane1 = sole::uuid4().str();
  sendPacket(h.handler, h.client->getEndpointFd(), NEW_TAB, tab1 + pane1);

  string tab2 = sole::uuid4().str();
  string pane2 = sole::uuid4().str();
  sendPacket(h.handler, h.client->getEndpointFd(), NEW_TAB, tab2 + pane2);

  string splitV = sole::uuid4().str();
  sendPacket(h.handler, h.client->getEndpointFd(), NEW_SPLIT,
             p0 + splitV + string("1"));
  string splitH = sole::uuid4().str();
  sendPacket(h.handler, h.client->getEndpointFd(), NEW_SPLIT,
             p0 + splitH + string("0"));

  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  const int bursts = 6;
  const vector<pair<string, string>> panes = {
      {p0, "SRV_C0"},     {pane1, "SRV_C1"},  {pane2, "SRV_C2"},
      {splitV, "SRV_C3"}, {splitH, "SRV_C4"},
  };
  for (const auto& pane : panes) {
    string cmd = "i=1; while [ \"$i\" -le " + to_string(bursts) +
                 " ]; do printf '" + pane.second +
                 "_%s\\n' \"$i\"; i=$((i+1)); sleep 0.04; done\n";
    sendInsertKeys(h.handler, h.client->getEndpointFd(), pane.first, cmd);
  }

  map<string, string> out;
  vector<string> order;
  REQUIRE(waitUntil(
      [&]() {
        h.incoming += readUntil(h.handler, h.client->getEndpointFd(), 1, 40);
        consumeInitSequence(&h.incoming);
        HtmPacket packet;
        string work = h.incoming;
        while (popPacket(&work, &packet)) {
          h.incoming = work;
          string id;
          string body;
          if (decodeAppendToPane(packet, &id, &body)) {
            out[id].append(body);
            order.push_back(id);
          }
        }
        for (const auto& pane : panes) {
          if (out[pane.first].find(pane.second + "_" + to_string(bursts)) ==
              string::npos) {
            return false;
          }
        }
        return true;
      },
      15000));

  for (const auto& pane : panes) {
    REQUIRE(out[pane.first].find(pane.second + "_1") != string::npos);
  }
  std::set<string> unique(order.begin(), order.end());
  REQUIRE(unique.size() >= 4);
  int transitions = 0;
  for (size_t i = 1; i < order.size(); i++) {
    if (order[i] != order[i - 1]) {
      transitions++;
    }
  }
  REQUIRE(transitions >= 4);

  sendDebugKeys(h.handler, h.client->getEndpointFd(), "x");
  h.stop();
}

TEST_CASE("HtmServer floods concurrent read/write on many panes",
          "[Htm][HtmServer][stress]") {
  HtmServerHarness h;
  json state = h.waitForInit();
  string p0 = firstJsonKey(state["panes"]);
  vector<pair<string, string>> panes = {{p0, "P0"}};
  for (int t = 0; t < 3; t++) {
    string tabPane = sole::uuid4().str();
    sendPacket(h.handler, h.client->getEndpointFd(), NEW_TAB,
               sole::uuid4().str() + tabPane);
    panes.push_back({tabPane, "T" + to_string(t)});
    string splitPane = sole::uuid4().str();
    sendPacket(h.handler, h.client->getEndpointFd(), NEW_SPLIT,
               tabPane + splitPane + string(t % 2 == 0 ? "1" : "0"));
    panes.push_back({splitPane, "S" + to_string(t)});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const int floodLines = 250;
  const int keyRounds = 16;
  for (const auto& pane : panes) {
    string cmd = "i=1; while [ \"$i\" -le " + to_string(floodLines) +
                 " ]; do printf 'OUT_" + pane.second +
                 "_%04d\\n' \"$i\"; i=$((i+1)); done &\n";
    sendInsertKeys(h.handler, h.client->getEndpointFd(), pane.first, cmd);
    h.incoming += readUntil(h.handler, h.client->getEndpointFd(), 1, 20);
  }
  for (int round = 0; round < keyRounds; round++) {
    for (const auto& pane : panes) {
      sendInsertKeys(
          h.handler, h.client->getEndpointFd(), pane.first,
          "printf 'IN_" + pane.second + "_" + to_string(round) + "\\n'\n");
    }
    h.incoming += readUntil(h.handler, h.client->getEndpointFd(), 1, 15);
  }

  map<string, string> out;
  REQUIRE(waitUntil(
      [&]() {
        h.incoming += readUntil(h.handler, h.client->getEndpointFd(), 1, 40);
        consumeInitSequence(&h.incoming);
        HtmPacket packet;
        string work = h.incoming;
        while (popPacket(&work, &packet)) {
          h.incoming = work;
          string id;
          string body;
          if (decodeAppendToPane(packet, &id, &body)) {
            out[id].append(body);
          }
        }
        for (const auto& pane : panes) {
          char lastOut[32];
          snprintf(lastOut, sizeof(lastOut), "OUT_%s_%04d", pane.second.c_str(),
                   floodLines);
          if (out[pane.first].find(lastOut) == string::npos ||
              out[pane.first].find("IN_" + pane.second + "_" +
                                   to_string(keyRounds - 1)) == string::npos) {
            return false;
          }
        }
        return true;
      },
      60000));

  sendDebugKeys(h.handler, h.client->getEndpointFd(), "x");
  h.stop();
}

TEST_CASE("HtmServer stops when the last pane is closed", "[Htm][HtmServer]") {
  HtmServerHarness h;
  json state = h.waitForInit();
  string paneId = firstJsonKey(state["panes"]);
  sendPacket(h.handler, h.client->getEndpointFd(), CLIENT_CLOSE_PANE, paneId);
  REQUIRE(waitUntil(
      [&]() { return !h.runner.joinable() || h.server.getEndpointFd() < 0; },
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
  REQUIRE(
      waitUntil([&]() { return waitpid(pid, &status, WNOHANG) == pid; }, 5000));
  close(inpipe[1]);
  close(outpipe[0]);
}

TEST_CASE("HtmClient does not treat a D-prefixed payload as SESSION_END",
          "[Htm][HtmClient][stress]") {
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

  const char payload[] = "DThisIsBase64ishPayloadNotSessionEnd";
  handler->writeAllOrThrow(server.getEndpointFd(), payload, sizeof(payload) - 1,
                           false);
  string fromStdout;
  REQUIRE(waitUntil(
      [&]() {
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(outpipe[0], &rfd);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        if (select(outpipe[0] + 1, &rfd, NULL, NULL, &tv) > 0) {
          char buf[64];
          ssize_t n = ::read(outpipe[0], buf, sizeof(buf));
          if (n > 0) {
            fromStdout.append(buf, size_t(n));
          }
        }
        return fromStdout.find(payload) != string::npos;
      },
      3000));

  REQUIRE(::write(inpipe[1], "z", 1) == 1);
  string fromClient = readUntil(handler, server.getEndpointFd(), 1, 3000);
  REQUIRE(fromClient.find('z') != string::npos);

  int status = 0;
  REQUIRE(waitpid(pid, &status, WNOHANG) != pid);

  server.closeEndpoint();
  REQUIRE(
      waitUntil([&]() { return waitpid(pid, &status, WNOHANG) == pid; }, 5000));
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
  REQUIRE(
      waitUntil([&]() { return waitpid(pid, &status, WNOHANG) == pid; }, 5000));
  close(inpipe[1]);
  close(outpipe[0]);
}

// Hyper-style deadlock: a blocked writeAll(stdout) used to stall the select
// loop so stdin was never read. Fill the stdout pipe from IPC, then keep
// injecting stdin without draining stdout and require those bytes to reach
// the server. Single-threaded so macOS unix sockets are not read+written
// from two threads at once.
TEST_CASE("HtmClient keeps forwarding stdin when stdout is backed up",
          "[Htm][HtmClient][stress]") {
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
  REQUIRE(fcntl(inpipe[1], F_SETFL, O_NONBLOCK) == 0);
  REQUIRE(fcntl(outpipe[0], F_SETFL, O_NONBLOCK) == 0);
  REQUIRE(waitUntil(
      [&]() {
        server.pollAccept();
        return server.getEndpointFd() >= 0;
      },
      5000));

  int ipcFd = server.getEndpointFd();
  int flags = fcntl(ipcFd, F_GETFL);
  REQUIRE(flags >= 0);
  REQUIRE(fcntl(ipcFd, F_SETFL, flags | O_NONBLOCK) == 0);

  auto waitReadable = [&](int fd, int timeoutMs) {
    fd_set rfd;
    FD_ZERO(&rfd);
    FD_SET(fd, &rfd);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return ::select(fd + 1, &rfd, NULL, NULL, &tv) > 0;
  };
  auto waitWritable = [&](int fd, int timeoutMs) {
    fd_set wfd;
    FD_ZERO(&wfd);
    FD_SET(fd, &wfd);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return ::select(fd + 1, NULL, &wfd, NULL, &tv) > 0;
  };

  string chunk(4096, 'A');
  const auto floodStart = std::chrono::steady_clock::now();
  size_t flooded = 0;
  while (flooded < 512 * 1024) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - floodStart)
                       .count();
    if (elapsed > 2000) {
      break;
    }
    if (!waitWritable(ipcFd, 20)) {
      break;
    }
    ssize_t n = ::write(ipcFd, chunk.data(), chunk.size());
    if (n > 0) {
      flooded += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      continue;
    }
    break;
  }
  REQUIRE(flooded > 0);

  int keysSeen = 0;
  const auto keyStart = std::chrono::steady_clock::now();
  const char keys[] = "kkkkkkkk";
  while (keysSeen < 64) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - keyStart)
                       .count();
    if (elapsed > 8000) {
      break;
    }
    ::write(inpipe[1], keys, sizeof(keys) - 1);
    if (!waitReadable(ipcFd, 20)) {
      continue;
    }
    char buf[1024];
    ssize_t n = ::read(ipcFd, buf, sizeof(buf));
    if (n > 0) {
      for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == 'k') {
          keysSeen++;
        }
      }
    }
  }
  REQUIRE(keysSeen >= 64);

  char drainBuf[4096];
  for (int i = 0; i < 80; i++) {
    if (!waitReadable(outpipe[0], 10)) {
      break;
    }
    ::read(outpipe[0], drainBuf, sizeof(drainBuf));
  }

  char sessionEnd = SESSION_END;
  for (int i = 0; i < 20; i++) {
    if (waitWritable(ipcFd, 20) && ::write(ipcFd, &sessionEnd, 1) == 1) {
      break;
    }
    if (waitReadable(outpipe[0], 10)) {
      ::read(outpipe[0], drainBuf, sizeof(drainBuf));
    }
  }

  int status = 0;
  if (!waitUntil([&]() { return waitpid(pid, &status, WNOHANG) == pid; },
                 3000)) {
    server.closeEndpoint();
    REQUIRE(waitUntil([&]() { return waitpid(pid, &status, WNOHANG) == pid; },
                      5000));
  }
  close(inpipe[1]);
  close(outpipe[0]);
}
#endif
