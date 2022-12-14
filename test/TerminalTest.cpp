#include <atomic>

#include "FakeConsole.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"
#include "TunnelUtils.hpp"

namespace et {
TEST_CASE("FakeConsoleTest", "[FakeConsoleTest]") {
  shared_ptr<PipeSocketHandler> socketHandler;
  shared_ptr<FakeConsole> fakeConsole;
  socketHandler.reset(new PipeSocketHandler());
  fakeConsole.reset(new FakeConsole(socketHandler));
  fakeConsole->setup();

  string s(64 * 1024, '\0');
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  REQUIRE(!socketHandler->hasData(fakeConsole->getFd()));

  thread t([fakeConsole, s]() { fakeConsole->simulateKeystrokes(s); });
  sleep(1);

  REQUIRE(socketHandler->hasData(fakeConsole->getFd()));

  string s2(64 * 1024, '\0');
  socketHandler->readAll(fakeConsole->getFd(), &s2[0], s2.length(), false);

  t.join();

  REQUIRE(s == s2);

  thread t2([fakeConsole, s]() { fakeConsole->write(s); });

  string s3 = fakeConsole->getTerminalData(s.length());
  REQUIRE(s == s3);

  t2.join();

  fakeConsole->teardown();
  fakeConsole.reset();
  socketHandler.reset();
}

TEST_CASE("FakeUserTerminalTest", "[FakeUserTerminalTest]") {
  shared_ptr<PipeSocketHandler> socketHandler;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;
  socketHandler.reset(new PipeSocketHandler());
  fakeUserTerminal.reset(new FakeUserTerminal(socketHandler));
  fakeUserTerminal->setup(-1);

  string s(64 * 1024, '\0');
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  thread t([fakeUserTerminal, s]() {
    RawSocketUtils::writeAll(fakeUserTerminal->getFd(), &s[0], s.length());
  });

  string s2 = fakeUserTerminal->getKeystrokes(s.length());
  REQUIRE(s == s2);
  t.join();

  REQUIRE(!socketHandler->hasData(fakeUserTerminal->getFd()));
  thread t2([fakeUserTerminal, s]() {
    fakeUserTerminal->simulateTerminalResponse(s);
  });

  string s3(64 * 1024, '\0');
  socketHandler->readAll(fakeUserTerminal->getFd(), &s3[0], s3.length(), false);

  t2.join();
  REQUIRE(s == s3);

  fakeUserTerminal->cleanup();
  fakeUserTerminal.reset();
  socketHandler.reset();
}

const string CRYPTO_KEY = "12345678901234567890123456789012";
const string CRYPTO_KEY2 = "98765432109876543210987654321098";

void readWriteTest(const string& clientId,
                   shared_ptr<PipeSocketHandler> routerSocketHandler,
                   shared_ptr<FakeUserTerminal> fakeUserTerminal,
                   SocketEndpoint serverEndpoint,
                   shared_ptr<SocketHandler> clientSocketHandler,
                   shared_ptr<SocketHandler> clientPipeSocketHandler,
                   shared_ptr<FakeConsole> fakeConsole,
                   const SocketEndpoint& routerEndpoint) {
  auto uth = shared_ptr<UserTerminalHandler>(
      new UserTerminalHandler(routerSocketHandler, fakeUserTerminal, true,
                              routerEndpoint, clientId + "/" + CRYPTO_KEY));
  thread uthThread([uth]() { uth->run(); });
  sleep(1);

  shared_ptr<TerminalClient> terminalClient(new TerminalClient(
      clientSocketHandler, clientPipeSocketHandler, serverEndpoint, clientId,
      CRYPTO_KEY, fakeConsole, false, "", "", false, "",
      MAX_CLIENT_KEEP_ALIVE_DURATION));
  thread terminalClientThread([terminalClient]() { terminalClient->run(""); });
  sleep(3);

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
      Catch::Matchers::Contains("must have source and destination"));
  REQUIRE_THROWS_WITH(parseRangesToRequests("6010-6012:7000"),
                      Catch::Matchers::Contains("must be a range"));
  REQUIRE_THROWS_WITH(parseRangesToRequests("6010:7000-7010"),
                      Catch::Matchers::Contains("must be a range"));
  REQUIRE_THROWS_WITH(parseRangesToRequests("6010-6012:7000-8000"),
                      Catch::Matchers::Contains("must have same length"));
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

TEST_CASE_METHOD(EndToEndTestFixture, "EndToEndTest", "[EndToEndTest]") {
  readWriteTest("1234567890123456", routerSocketHandler, fakeUserTerminal,
                serverEndpoint, clientSocketHandler, clientPipeSocketHandler,
                fakeConsole, routerEndpoint);
}

void simultaneousTerminalConnectionTest(
    const string& clientId, const string& simultaneousTerminalPasskey,
    LogInterceptHandler& logInterceptHandler,
    shared_ptr<PipeSocketHandler> routerSocketHandler,
    shared_ptr<PipeSocketHandler> userTerminalSocketHandler,
    shared_ptr<FakeUserTerminal> fakeUserTerminal,
    SocketEndpoint serverEndpoint,
    shared_ptr<SocketHandler> clientSocketHandler,
    shared_ptr<SocketHandler> clientPipeSocketHandler,
    shared_ptr<FakeConsole> fakeConsole, const SocketEndpoint& routerEndpoint) {
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
                              routerEndpoint, clientId + "/" + CRYPTO_KEY));

  constexpr int kNumSimultaneousTerminals = 4;
  std::vector<SimultaneousTerminalState> otherTerminals;
  for (int i = 0; i < kNumSimultaneousTerminals; ++i) {
    otherTerminals.emplace_back(clientId, simultaneousTerminalPasskey,
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

  shared_ptr<TerminalClient> terminalClient(new TerminalClient(
      clientSocketHandler, clientPipeSocketHandler, serverEndpoint, clientId,
      CRYPTO_KEY, fakeConsole, false, "", "", false, "",
      MAX_CLIENT_KEEP_ALIVE_DURATION));
  thread terminalClientThread([terminalClient]() { terminalClient->run(""); });
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
                 "[EndToEndTest]") {
  SECTION("Valid passkey") {
    simultaneousTerminalConnectionTest(
        "1234567890123456", CRYPTO_KEY, logInterceptHandler,
        routerSocketHandler, userTerminalSocketHandler, fakeUserTerminal,
        serverEndpoint, clientSocketHandler, clientPipeSocketHandler,
        fakeConsole, routerEndpoint);
  }

  SECTION("Different passkey") {
    simultaneousTerminalConnectionTest(
        "1234567890123456", CRYPTO_KEY2, logInterceptHandler,
        routerSocketHandler, userTerminalSocketHandler, fakeUserTerminal,
        serverEndpoint, clientSocketHandler, clientPipeSocketHandler,
        fakeConsole, routerEndpoint);
  }
}

// TODO: Multiple clients

// TODO: FlakySocket
}  // namespace et
