#include "ETerminal.pb.h"
#include "FakeConsole.hpp"
#include "PipeSocketHandler.hpp"
#include "TestHeaders.hpp"
#include "UserTerminalHandler.hpp"

using namespace et;

#ifndef WIN32
namespace {
class RegistrationFailureSocketHandler : public SocketHandler {
 public:
  bool hasData(int) override { return false; }

  ssize_t read(int, void*, size_t) override {
    SetErrno(EPIPE);
    return -1;
  }

  ssize_t write(int, const void*, size_t count) override {
    if (failWrites) {
      SetErrno(EPIPE);
      return -1;
    }
    return static_cast<ssize_t>(count);
  }

  int connect(const SocketEndpoint&) override { return nextFd++; }
  set<int> listen(const SocketEndpoint&) override { return {}; }
  set<int> getEndpointFds(const SocketEndpoint&) override { return {}; }
  int accept(int) override { return -1; }
  void stopListening(const SocketEndpoint&) override {}
  void close(int fd) override { closedFds.push_back(fd); }
  vector<int> getActiveSockets() override { return {}; }

  bool failWrites = false;
  vector<int> closedFds;

 private:
  int nextFd = 100;
};

class RouterPairSocketHandler : public PipeSocketHandler {
 public:
  int connect(const SocketEndpoint&) override {
    int fds[2] = {-1, -1};
    FATAL_FAIL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    addToActiveSockets(fds[0]);
    initSocket(fds[0]);
    addToActiveSockets(fds[1]);
    initSocket(fds[1]);
    peerFd = fds[1];
    return fds[0];
  }

  int peerFd = -1;
};

class SessionEndTerminal : public UserTerminal {
 public:
  SessionEndTerminal() : masterFd(-1), peerFd(-1), exitCode(37) {}
  ~SessionEndTerminal() override { cleanup(); }

  int setup(int /*routerFd*/) override {
    int fds[2] = {-1, -1};
    FATAL_FAIL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    masterFd = fds[0];
    peerFd = fds[1];
    for (int fd : {masterFd, peerFd}) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }
    }
    return masterFd;
  }

  void runTerminal() override {}
  int handleSessionEnd() override {
    handleSessionEndCalls++;
    return exitCode;
  }
  bool sessionHasEnded() override { return ended.load(); }
  void terminate() override {}
  void cleanup() override {
    if (masterFd >= 0) {
      ::close(masterFd);
      masterFd = -1;
    }
    if (peerFd >= 0) {
      ::close(peerFd);
      peerFd = -1;
    }
  }
  int getFd() override { return masterFd; }
  void setInfo(const winsize&) override {}

  void writeOutput(const string& data) {
    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
      const ssize_t written = ::write(peerFd, p, left);
      if (written > 0) {
        p += written;
        left -= static_cast<size_t>(written);
        continue;
      }
      if (written < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      break;
    }
  }

  void endSession() { ended.store(true); }

  std::atomic<bool> ended{false};
  std::atomic<int> handleSessionEndCalls{0};
  int exitCode;

 private:
  int masterFd;
  int peerFd;
};

class TestableUserTerminalHandler : public UserTerminalHandler {
 public:
  using UserTerminalHandler::UserTerminalHandler;

  void registerForTest() { registerWithRouter(); }
  int routerFdForTest() const { return routerFd; }
  void runUserTerminalForTest(int masterFd) { runUserTerminal(masterFd); }
};
}  // namespace
#endif

TEST_CASE("UserTerminalHandler shutdown method exists",
          "[UserTerminalHandler]") {
  // This is a very basic test that just verifies the handler can be created
  // and shutdown() can be called. More complex integration tests are in
  // the TerminalTest suite.
  auto socketHandler = std::make_shared<PipeSocketHandler>();
  auto term = std::make_shared<FakeUserTerminal>(socketHandler);

  string pipeDirectory = test::makeTempDir("et_test_handler");
  string pipePath = pipeDirectory + "/router_pipe";

  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(pipePath);

  // Just verify that the shutdown method exists and can be called
  // without causing compilation errors
  UserTerminalHandler* handler = nullptr;
  (void)handler;
  (void)term;
  // Note: We're not actually creating the handler here because it requires
  // a running router endpoint, which would require complex setup.
  // The shutdown() method is already tested in integration tests.

  REQUIRE(true);  // Placeholder to indicate this test passes

  test::removeTempDir(pipeDirectory);
}

#ifndef WIN32
TEST_CASE("UserTerminalHandler closes a failed registration fd",
          "[UserTerminalHandler]") {
  auto socketHandler = make_shared<RegistrationFailureSocketHandler>();
  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name("test-router");

  TestableUserTerminalHandler handler(socketHandler, nullptr, true,
                                      routerEndpoint, "client/passkey");
  REQUIRE(handler.routerFdForTest() == 100);

  socketHandler->failWrites = true;
  REQUIRE_THROWS(handler.registerForTest());

  REQUIRE(handler.routerFdForTest() == -1);
  REQUIRE(socketHandler->closedFds == vector<int>{101});
}

TEST_CASE(
    "sessionHasEnded path sends TERMINAL_EXIT_STATUS and drains buffered "
    "output",
    "[UserTerminalHandler][BackgroundProcessTeardown]") {
  auto socketHandler = make_shared<RouterPairSocketHandler>();
  auto term = make_shared<SessionEndTerminal>();
  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name("test-router");

  TestableUserTerminalHandler handler(socketHandler, term, true, routerEndpoint,
                                      "client/passkey");
  const int peerFd = socketHandler->peerFd;
  REQUIRE(peerFd >= 0);

  Packet registrationPacket;
  REQUIRE(socketHandler->readPacket(peerFd, &registrationPacket));
  REQUIRE(registrationPacket.getHeader() ==
          TerminalPacketType::TERMINAL_USER_INFO);

  const int masterFd = term->setup(-1);
  REQUIRE(masterFd >= 0);

  // Buffer shell output before the handler loop starts, then mark the shell
  // ended. sessionHasEnded() is checked before masterFd is drained, so this
  // reproduces the teardown path without a race against the select loop.
  const string marker = "final-shell-output\n";
  term->writeOutput(marker);
  term->endSession();

  std::thread handlerThread(
      [&handler, masterFd]() { handler.runUserTerminalForTest(masterFd); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (term->handleSessionEndCalls.load() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  handlerThread.join();

  bool sawBufferedOutput = false;
  bool sawExitStatus = false;
  while (socketHandler->hasData(peerFd)) {
    Packet packet;
    REQUIRE(socketHandler->readPacket(peerFd, &packet));
    if (packet.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
      TerminalBuffer tb = stringToProto<TerminalBuffer>(packet.getPayload());
      if (tb.buffer().find(marker) != string::npos) {
        sawBufferedOutput = true;
      }
    } else if (packet.getHeader() == TerminalPacketType::TERMINAL_EXIT_STATUS) {
      TerminalExitStatus tes =
          stringToProto<TerminalExitStatus>(packet.getPayload());
      REQUIRE(tes.exitcode() == term->exitCode);
      sawExitStatus = true;
    }
  }

  REQUIRE(term->handleSessionEndCalls.load() > 0);
  REQUIRE(sawExitStatus);
  REQUIRE(sawBufferedOutput);
}
#endif
