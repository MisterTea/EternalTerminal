#include <atomic>
#include <chrono>
#include <future>

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#else
#include <pty.h>
#endif
#include <fcntl.h>
#include <sys/wait.h>
#include <termios.h>

#include "FakeConsole.hpp"
#include "FakeSshSetupHandler.hpp"
#include "SshSetupHandler.hpp"
#include "SubprocessUtils.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"
#include "TunnelUtils.hpp"

namespace et {

void waitForFakeConsoleSetup(const shared_ptr<FakeConsole>& fakeConsole) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fakeConsole->isSetup()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(fakeConsole->isSetup());
}

void readWriteTest(shared_ptr<PipeSocketHandler> routerSocketHandler,
                   shared_ptr<FakeUserTerminal> fakeUserTerminal,
                   SocketEndpoint serverEndpoint,
                   shared_ptr<SocketHandler> clientSocketHandler,
                   shared_ptr<SocketHandler> clientPipeSocketHandler,
                   shared_ptr<FakeConsole> fakeConsole,
                   const SocketEndpoint& routerEndpoint) {
  // Use SshSetupHandler with a fake SubprocessUtils to get id/passkey.
  auto fakeSubprocessUtils = make_shared<FakeSubprocessUtils>();
  auto sshSetupHandler = make_shared<FakeSshSetupHandler>(fakeSubprocessUtils);
  auto [id, passkey] = sshSetupHandler->SetupSsh(
      "", "localhost", "localhost", 2022, "", "", false, 0, "", "", {});

  auto uth = shared_ptr<UserTerminalHandler>(
      new UserTerminalHandler(routerSocketHandler, fakeUserTerminal, true,
                              routerEndpoint, id + "/" + passkey));
  thread uthThread([uth]() { uth->run(); });
  sleep(1);

  vector<pair<string, string>> envVars = {
      {"ET_TEST_VAR1", "test_value_1"},
      {"ET_TEST_VAR2_EMPTY", ""},
  };

  shared_ptr<TerminalClient> terminalClient(new TerminalClient(
      clientSocketHandler, clientPipeSocketHandler, serverEndpoint, id, passkey,
      fakeConsole, false, "", "", false, "", MAX_CLIENT_KEEP_ALIVE_DURATION,
      envVars));
  thread terminalClientThread(
      [terminalClient]() { terminalClient->run("", false); });
  sleep(3);
  waitForFakeConsoleSetup(fakeConsole);

  string s(1024, '\0');
  for (int a = 0; a < 1024; a++) {
    s[a] = rand() % 26 + 'A';
  }

  thread typeKeysThread([s, fakeConsole]() {
    for (int a = 0; a < 1024; a++) {
      VLOG(1) << "Writing packet " << a;
      fakeConsole->simulateKeystrokes(string(1, s[a]));
    }
  });

  string resultConcat;
  string result;
  for (int a = 0; a < 1024; a++) {
    result = fakeUserTerminal->getKeystrokes(1);
    resultConcat = resultConcat.append(result);
    LOG(INFO) << "ON MESSAGE " << a;
  }
  typeKeysThread.join();

  REQUIRE(resultConcat == s);

  const char* var1 = getenv("ET_TEST_VAR1");
  REQUIRE(var1 != nullptr);
  REQUIRE(string(var1) == "test_value_1");
  const char* var2 = getenv("ET_TEST_VAR2_EMPTY");
  REQUIRE(var2 != nullptr);
  REQUIRE(string(var2) == "");

  terminalClient->shutdown();
  terminalClientThread.join();
  terminalClient.reset();

  uth->shutdown();
  uthThread.join();
  uth.reset();

  unsetenv("ET_TEST_VAR1");
  unsetenv("ET_TEST_VAR2_EMPTY");
}

// A UserTerminal backed by a *real* pty running `cat`, so the test exercises
// real pty buffer + backpressure semantics.  FakeUserTerminal is a socket pair
// and cannot reproduce the input-write deadlock -- which is exactly why that
// bug went unnoticed.  `cat` echoes input back byte-for-byte; the pty is raw +
// no-echo so the round-trip is exactly 1:1.
class RealPtyEchoTerminal : public UserTerminal {
 public:
  RealPtyEchoTerminal() : masterFd(-1), childPid(-1) {}
  virtual ~RealPtyEchoTerminal() {}

  virtual int setup(int routerFd) {
    struct termios tios;
    memset(&tios, 0, sizeof(tios));
    cfmakeraw(&tios);
    tios.c_cc[VMIN] = 1;
    tios.c_cc[VTIME] = 0;
    childPid = forkpty(&masterFd, NULL, &tios, NULL);
    if (childPid == -1) {
      FATAL_FAIL(childPid);
    }
    if (childPid == 0) {
      execl("/bin/cat", "cat", (char*)NULL);
      _exit(127);
    }
    // Honor the UserTerminal contract: the handler polls this fd non-blocking.
    int flags = fcntl(masterFd, F_GETFL, 0);
    if (flags != -1) {
      fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
    }
    return masterFd;
  }
  virtual void runTerminal() {}
  virtual void handleSessionEnd() {}
  virtual void cleanup() {
    if (masterFd >= 0) {
      close(masterFd);
      masterFd = -1;
    }
    if (childPid > 0) {
      int status = 0;
      waitpid(childPid, &status, 0);
      childPid = -1;
    }
  }
  virtual int getFd() { return masterFd; }
  virtual void setInfo(const winsize& tmpwin) {}

 private:
  int masterFd;
  pid_t childPid;
};

// Pushes a payload much larger than one pty buffer through the real handler and
// a real pty, and requires that every byte echoes back within a deadline.  The
// pre-fix handler wrote input with a *blocking* writeAll(masterFd, ...); while
// blocked there it stopped draining output, so the echo filled the pty output
// buffer, the shell stalled, and the input write never completed -- a deadlock
// that hung past ~one pty buffer of input.  This test times out on that code
// and passes with the buffered non-blocking input drain.
void largeInputNoDeadlockTest(shared_ptr<PipeSocketHandler> routerSocketHandler,
                              SocketEndpoint serverEndpoint,
                              shared_ptr<SocketHandler> clientSocketHandler,
                              shared_ptr<SocketHandler> clientPipeSocketHandler,
                              shared_ptr<FakeConsole> fakeConsole,
                              const SocketEndpoint& routerEndpoint) {
  auto fakeSubprocessUtils = make_shared<FakeSubprocessUtils>();
  auto sshSetupHandler = make_shared<FakeSshSetupHandler>(fakeSubprocessUtils);
  auto [id, passkey] = sshSetupHandler->SetupSsh(
      "", "localhost", "localhost", 2022, "", "", false, 0, "", "", {});

  auto realPty = make_shared<RealPtyEchoTerminal>();
  auto uth = shared_ptr<UserTerminalHandler>(new UserTerminalHandler(
      routerSocketHandler, realPty, true, routerEndpoint, id + "/" + passkey));
  thread uthThread([uth]() { uth->run(); });
  sleep(1);

  shared_ptr<TerminalClient> terminalClient(
      new TerminalClient(clientSocketHandler, clientPipeSocketHandler,
                         serverEndpoint, id, passkey, fakeConsole, false, "",
                         "", false, "", MAX_CLIENT_KEEP_ALIVE_DURATION, {}));
  thread terminalClientThread(
      [terminalClient]() { terminalClient->run("", false); });
  sleep(3);
  waitForFakeConsoleSetup(fakeConsole);

  // Before the bulk test, confirm the full client -> pty(`cat`) -> client echo
  // pipe is actually live, and drain anything the connection produced during
  // setup.  The fixed sleeps above are not a reliable readiness signal on a
  // slow build -- under a sanitizer run with `ctest --parallel`, typing could
  // start before the pipeline was fully wired, which made this test flaky in
  // CI.  The sentinel is far smaller than one pty buffer, so it round-trips
  // even on the pre-fix handler; only the large payload below can trigger the
  // deadlock, so the warmup cannot mask a regression.
  const string sentinel = "ET_WARMUP_SENTINEL";
  fakeConsole->simulateKeystrokes(sentinel);
  {
    std::promise<bool> warmPromise;
    auto warmFuture = warmPromise.get_future();
    thread warmThread([&warmPromise, fakeConsole, sentinel]() {
      // Read one byte at a time until the rolling tail matches the sentinel,
      // discarding any bytes that preceded the echo.
      string seen;
      while (seen.size() < sentinel.size() ||
             seen.compare(seen.size() - sentinel.size(), sentinel.size(),
                          sentinel) != 0) {
        seen += fakeConsole->getTerminalData(1);
      }
      warmPromise.set_value(true);
    });
    bool warm = warmFuture.wait_for(std::chrono::seconds(30)) ==
                std::future_status::ready;
    REQUIRE(warm);  // the echo pipe must be live before the bulk test
    if (warm) {
      warmThread.join();
    } else {
      warmThread.detach();
    }
  }

  // A payload well past one pty buffer (~1KB macOS, ~8KB Linux) -- enough to
  // deadlock the pre-fix handler -- but small enough not to stress the
  // pipe-based test transport.
  const int kSize = 8 * 1024;
  string payload(kSize, '\0');
  for (int a = 0; a < kSize; a++) {
    payload[a] = 'a' + (rand() % 26);
  }

  thread typeThread([payload, fakeConsole]() {
    for (size_t off = 0; off < payload.size(); off += 1024) {
      fakeConsole->simulateKeystrokes(payload.substr(off, 1024));
    }
  });

  // Read the echo back under a watchdog; the deadlocking code never delivers
  // it.  The deadline is generous so a slow (sanitizer) build still cleanly
  // distinguishes a wedge (never delivers) from success, without flaking.
  std::promise<string> donePromise;
  auto doneFuture = donePromise.get_future();
  thread readThread([&donePromise, fakeConsole, kSize]() {
    donePromise.set_value(fakeConsole->getTerminalData(kSize));
  });

  bool completed = doneFuture.wait_for(std::chrono::seconds(60)) ==
                   std::future_status::ready;
  REQUIRE(completed);  // would time out (FAIL) on the pre-fix deadlock

  if (completed) {
    string got = doneFuture.get();
    REQUIRE(got.size() == (size_t)kSize);
    REQUIRE(got == payload);
    typeThread.join();
    readThread.join();
  } else {
    // Don't hang the suite on a regression; teardown unblocks the threads.
    typeThread.detach();
    readThread.detach();
  }

  terminalClient->shutdown();
  terminalClientThread.join();
  terminalClient.reset();

  uth->shutdown();
  uthThread.join();
  uth.reset();
}

class LogInterceptHandler : public el::LogDispatchCallback {
 public:
  void handle(const el::LogDispatchData* data) {
    const std::string& message = data->logMessage()->message();

    std::function<void()> callback;
    {
      lock_guard<mutex> lock(classMutex);

      if (!wasHit && message.size() >= interceptPrefix.size() &&
          message.substr(0, interceptPrefix.size()) == interceptPrefix) {
        wasHit = true;
        callback = std::move(interceptCallback);
      }
    }

    if (callback) {
      callback();
    }
  }

  void setIntercept(string prefix, std::function<void()> callback) {
    lock_guard<mutex> lock(classMutex);
    wasHit = false;
    interceptPrefix = prefix;
    interceptCallback = callback;
  }

 private:
  mutex classMutex;
  // Default to true to disable the matcher until setIntercept is called.
  bool wasHit = true;
  string interceptPrefix;
  std::function<void()> interceptCallback;
};

class EndToEndTestFixture {
 public:
  EndToEndTestFixture() {
    el::Helpers::installLogDispatchCallback<LogInterceptHandler>(
        "LogInterceptHandler");

    srand(1);
    clientSocketHandler.reset(new PipeSocketHandler());
    clientPipeSocketHandler.reset(new PipeSocketHandler());
    serverSocketHandler.reset(new PipeSocketHandler());
    routerSocketHandler.reset(new PipeSocketHandler());
    el::Helpers::setThreadName("Main");
    consoleSocketHandler.reset(new PipeSocketHandler());
    fakeConsole.reset(new FakeConsole(consoleSocketHandler));

    userTerminalSocketHandler.reset(new PipeSocketHandler());
    fakeUserTerminal.reset(new FakeUserTerminal(userTerminalSocketHandler));

    string tmpPath = GetTempDirectory() + string("etserver_test_XXXXXXXX");
    pipeDirectory = string(mkdtemp(&tmpPath[0]));

    routerPipePath = string(pipeDirectory) + "/pipe_router";
    routerEndpoint.set_name(routerPipePath);

    serverPipePath = string(pipeDirectory) + "/pipe_server";
    serverEndpoint.set_name(serverPipePath);

    server = shared_ptr<TerminalServer>(
        new TerminalServer(serverSocketHandler, serverEndpoint,
                           routerSocketHandler, routerEndpoint));
    serverThread = thread([this]() { server->run(); });
    sleep(1);
  }

  ~EndToEndTestFixture() {
    server->shutdown();
    serverThread.join();

    consoleSocketHandler.reset();
    userTerminalSocketHandler.reset();
    serverSocketHandler.reset();
    clientSocketHandler.reset();
    clientPipeSocketHandler.reset();
    routerSocketHandler.reset();
    FATAL_FAIL(::remove(routerPipePath.c_str()));
    FATAL_FAIL(::remove(serverPipePath.c_str()));
    FATAL_FAIL(::remove(pipeDirectory.c_str()));

    el::Helpers::uninstallLogDispatchCallback<LogInterceptHandler>(
        "LogInterceptHandler");
  }

 protected:
  LogInterceptHandler logInterceptHandler;

  shared_ptr<PipeSocketHandler> consoleSocketHandler;
  shared_ptr<PipeSocketHandler> userTerminalSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;

  shared_ptr<SocketHandler> serverSocketHandler;
  shared_ptr<SocketHandler> clientSocketHandler;
  shared_ptr<SocketHandler> clientPipeSocketHandler;

  string pipeDirectory;

  SocketEndpoint serverEndpoint;
  string serverPipePath;

  SocketEndpoint routerEndpoint;
  string routerPipePath;

  shared_ptr<FakeConsole> fakeConsole;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;

  shared_ptr<TerminalServer> server;
  thread serverThread;

 private:
  bool wasShutdown = false;
};

TEST_CASE("InvalidTunnelArgParsing", "[InvalidTunnelArgParsing]") {
  REQUIRE_THROWS_WITH(
      parseRangesToRequests("6010"),
      Catch::Matchers::ContainsSubstring("must have source and destination"));
  REQUIRE_THROWS_WITH(parseRangesToRequests("6010-6012:7000"),
                      Catch::Matchers::ContainsSubstring("must be a range"));
  REQUIRE_THROWS_WITH(parseRangesToRequests("6010:7000-7010"),
                      Catch::Matchers::ContainsSubstring("must be a range"));
  REQUIRE_THROWS_WITH(
      parseRangesToRequests("6010-6012:7000-8000"),
      Catch::Matchers::ContainsSubstring("must have same length"));
}

TEST_CASE("ValidTunnelArgParsing", "[ValidTunnelArgParsing]") {
  // Plain port1:port2 forward
  auto pfsrs_single = parseRangesToRequests("6010:7010");
  REQUIRE(pfsrs_single.size() == 1);
  REQUIRE(pfsrs_single[0].has_source());
  REQUIRE(pfsrs_single[0].has_destination());
  REQUIRE((pfsrs_single[0].source().has_port() &&
           pfsrs_single[0].source().port() == 6010));
  REQUIRE((pfsrs_single[0].destination().has_port() &&
           pfsrs_single[0].destination().port() == 7010));

  // range src_port1-src_port2:dest_port1-dest_port2 forward
  auto pfsrs_ranges = parseRangesToRequests("6010-6013:7010-7013");
  REQUIRE(pfsrs_ranges.size() == 4);

  // named pipe forward
  auto pfsrs_named = parseRangesToRequests("envvar:/tmp/destination");
  REQUIRE(pfsrs_named.size() == 1);
  REQUIRE(!pfsrs_named[0].has_source());
  REQUIRE(pfsrs_named[0].has_destination());
  REQUIRE(pfsrs_named[0].has_environmentvariable());
}

TEST_CASE_METHOD(EndToEndTestFixture, "EndToEndTest",
                 "[EndToEndTest][integration]") {
  readWriteTest(routerSocketHandler, fakeUserTerminal, serverEndpoint,
                clientSocketHandler, clientPipeSocketHandler, fakeConsole,
                routerEndpoint);
}

TEST_CASE_METHOD(EndToEndTestFixture, "LargeInputNoDeadlock",
                 "[EndToEndTest][integration]") {
  largeInputNoDeadlockTest(routerSocketHandler, serverEndpoint,
                           clientSocketHandler, clientPipeSocketHandler,
                           fakeConsole, routerEndpoint);
}

void simultaneousTerminalConnectionTest(
    LogInterceptHandler& logInterceptHandler,
    shared_ptr<PipeSocketHandler> routerSocketHandler,
    shared_ptr<PipeSocketHandler> userTerminalSocketHandler,
    shared_ptr<FakeUserTerminal> fakeUserTerminal,
    SocketEndpoint serverEndpoint,
    shared_ptr<SocketHandler> clientSocketHandler,
    shared_ptr<SocketHandler> clientPipeSocketHandler,
    shared_ptr<FakeConsole> fakeConsole, const SocketEndpoint& routerEndpoint) {
  // Use SshSetupHandler with a fake SubprocessUtils to get id/passkey.
  auto fakeSubprocessUtils = make_shared<FakeSubprocessUtils>();
  SshSetupHandler sshSetupHandler(fakeSubprocessUtils);
  auto [id, passkey] = sshSetupHandler.SetupSsh(
      "", "localhost", "localhost", 2022, "", "", false, 0, "", "", {});

  // Get id/passkey for simultaneous terminals (use different credentials)
  auto fakeSubprocessUtilsSimultaneous = make_shared<FakeSubprocessUtils>();
  SshSetupHandler sshSetupHandlerSimultaneous(fakeSubprocessUtilsSimultaneous);
  auto [simultaneousId, simultaneousPasskey] =
      sshSetupHandlerSimultaneous.SetupSsh("", "localhost", "localhost", 2022,
                                           "", "", false, 0, "", "", {});

  struct SimultaneousTerminalState {
    const string& clientId;
    const string& passkey;
    shared_ptr<PipeSocketHandler> routerSocketHandler;
    shared_ptr<PipeSocketHandler> userTerminalSocketHandler;
    shared_ptr<FakeUserTerminal> fakeUserTerminal;
    const SocketEndpoint& routerEndpoint;

    shared_ptr<UserTerminalHandler> handler;
    thread handlerThread;

    SimultaneousTerminalState(
        const string& _clientId, const string& _passkey,
        shared_ptr<PipeSocketHandler> _routerSocketHandler,
        shared_ptr<PipeSocketHandler> _userTerminalSocketHandler,
        shared_ptr<FakeUserTerminal> _fakeUserTerminal,
        const SocketEndpoint& _routerEndpoint)
        : clientId(_clientId),
          passkey(_passkey),
          routerSocketHandler(_routerSocketHandler),
          userTerminalSocketHandler(_userTerminalSocketHandler),
          fakeUserTerminal(_fakeUserTerminal),
          routerEndpoint(_routerEndpoint) {}

    // Move-only.
    SimultaneousTerminalState(const SimultaneousTerminalState&) = delete;
    SimultaneousTerminalState(SimultaneousTerminalState&&) = default;

    void start() {
      handler.reset(
          new UserTerminalHandler(routerSocketHandler, fakeUserTerminal, true,
                                  routerEndpoint, clientId + "/" + passkey));
      handlerThread = thread([this]() { CHECK_THROWS(handler->run()); });
    }

    ~SimultaneousTerminalState() {
      if (handler) {
        handler->shutdown();
        if (handlerThread.joinable()) {
          handlerThread.join();
        }
      }
    }
  };

  auto uth = shared_ptr<UserTerminalHandler>(
      new UserTerminalHandler(routerSocketHandler, fakeUserTerminal, true,
                              routerEndpoint, id + "/" + passkey));

  constexpr int kNumSimultaneousTerminals = 4;
  std::vector<SimultaneousTerminalState> otherTerminals;
  for (int i = 0; i < kNumSimultaneousTerminals; ++i) {
    otherTerminals.emplace_back(simultaneousId, simultaneousPasskey,
                                routerSocketHandler, userTerminalSocketHandler,
                                fakeUserTerminal, routerEndpoint);
  }

  logInterceptHandler.setIntercept("Got client with id: ", [&otherTerminals]() {
    // Try to create more terminals while the main terminal is connecting.
    // NOTE: There must be no logging within this handler,
    for (auto& terminal : otherTerminals) {
      terminal.start();
    }
  });

  thread uthThread([uth]() { REQUIRE_NOTHROW(uth->run()); });
  sleep(1);

  shared_ptr<TerminalClient> terminalClient(
      new TerminalClient(clientSocketHandler, clientPipeSocketHandler,
                         serverEndpoint, id, passkey, fakeConsole, false, "",
                         "", false, "", MAX_CLIENT_KEEP_ALIVE_DURATION, {}));
  thread terminalClientThread(
      [terminalClient]() { terminalClient->run("", false); });
  sleep(3);

  const string s("test");
  thread typeKeysThread([s, fakeConsole]() {
    for (int a = 0; a < s.size(); a++) {
      VLOG(1) << "Writing packet " << a;
      fakeConsole->simulateKeystrokes(string(1, s[a]));
    }
  });

  string resultConcat;
  string result;
  for (int a = 0; a < s.size(); a++) {
    result = fakeUserTerminal->getKeystrokes(1);
    resultConcat = resultConcat.append(result);
    LOG(INFO) << "ON MESSAGE " << a;
  }
  typeKeysThread.join();

  REQUIRE(resultConcat == s);

  terminalClient->shutdown();
  terminalClientThread.join();
  terminalClient.reset();

  uth->shutdown();
  uthThread.join();
  uth.reset();

  otherTerminals.clear();
}

TEST_CASE_METHOD(EndToEndTestFixture, "TerminalConnectSimultaneous",
                 "[EndToEndTest][integration]") {
  SECTION("Valid passkey") {
    simultaneousTerminalConnectionTest(
        logInterceptHandler, routerSocketHandler, userTerminalSocketHandler,
        fakeUserTerminal, serverEndpoint, clientSocketHandler,
        clientPipeSocketHandler, fakeConsole, routerEndpoint);
  }

  SECTION("Different passkey") {
    simultaneousTerminalConnectionTest(
        logInterceptHandler, routerSocketHandler, userTerminalSocketHandler,
        fakeUserTerminal, serverEndpoint, clientSocketHandler,
        clientPipeSocketHandler, fakeConsole, routerEndpoint);
  }
}

// TODO: Multiple clients

// TODO: FlakySocket
}  // namespace et
