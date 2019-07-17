#include "TestHeaders.hpp"

#include "FakeConsole.hpp"
#include "Terminal.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"

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
  usleep(1000);

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
  usleep(1000);
  REQUIRE(socketHandler->hasData(fakeUserTerminal->getFd()));

  string s3(64 * 1024, '\0');
  socketHandler->readAll(fakeUserTerminal->getFd(), &s3[0], s3.length(), false);

  t2.join();
  REQUIRE(s == s3);

  fakeUserTerminal->cleanup();
  fakeUserTerminal.reset();
  socketHandler.reset();
}

const string CRYPTO_KEY = "12345678901234567890123456789012";

void readWriteTest(const string& clientId,
                   shared_ptr<PipeSocketHandler> routerSocketHandler,
                   shared_ptr<FakeUserTerminal> fakeUserTerminal,
                   SocketEndpoint serverEndpoint,
                   shared_ptr<SocketHandler> clientSocketHandler,
                   shared_ptr<FakeConsole> fakeConsole) {
  thread t_terminal([routerSocketHandler, fakeUserTerminal, clientId]() {
    et::startUserTerminal(routerSocketHandler, fakeUserTerminal,
                          clientId + CRYPTO_KEY, true);
  });

  shared_ptr<TerminalClient> terminalClient(new TerminalClient(
      clientSocketHandler, serverEndpoint, clientId, CRYPTO_KEY, fakeConsole));

  const int NUM_MESSAGES = 32;
  string s(NUM_MESSAGES * 1024, '\0');
  for (int a = 0; a < NUM_MESSAGES * 1024; a++) {
    s[a] = rand() % 26 + 'A';
  }

  for (int a = 0; a < NUM_MESSAGES; a++) {
    VLOG(1) << "Writing packet " << a;
    fakeConsole->simulateKeystrokes(string((&s[0] + a * 1024)));
  }

  string resultConcat;
  string result;
  for (int a = 0; a < NUM_MESSAGES; a++) {
    result = fakeUserTerminal->getKeystrokes(1024);
    resultConcat = resultConcat.append(result);
    LOG(INFO) << "ON MESSAGE " << a;
  }

  REQUIRE(resultConcat == s);

  // TODO: reverse order
}

TEST_CASE("EndToEndTest", "[EndToEndTest]") {
  shared_ptr<PipeSocketHandler> consoleSocketHandler;
  shared_ptr<PipeSocketHandler> userTerminalSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;

  shared_ptr<SocketHandler> serverSocketHandler;
  shared_ptr<SocketHandler> clientSocketHandler;

  SocketEndpoint serverEndpoint;

  shared_ptr<FakeConsole> fakeConsole;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;

  string pipeDirectory;
  string pipePath;

  srand(1);
  clientSocketHandler.reset(new PipeSocketHandler());
  serverSocketHandler.reset(new PipeSocketHandler());
  routerSocketHandler.reset(new PipeSocketHandler());
  el::Helpers::setThreadName("Main");
  consoleSocketHandler.reset(new PipeSocketHandler());
  fakeConsole.reset(new FakeConsole(consoleSocketHandler));
  fakeConsole->setup();

  userTerminalSocketHandler.reset(new PipeSocketHandler());
  fakeUserTerminal.reset(new FakeUserTerminal(userTerminalSocketHandler));
  fakeUserTerminal->setup(-1);

  string tmpPath = string("/tmp/etserver_test_XXXXXXXX");
  pipeDirectory = string(mkdtemp(&tmpPath[0]));
  pipePath = string(pipeDirectory) + "/pipe";
  serverEndpoint = SocketEndpoint(pipePath);

  thread t_server([serverSocketHandler, serverEndpoint, routerSocketHandler]() {
    et::startServer(serverSocketHandler, serverEndpoint, routerSocketHandler);
  });

  readWriteTest("1234567890123456", routerSocketHandler, fakeUserTerminal,
                serverEndpoint, clientSocketHandler, fakeConsole);
  t_server.join();

  consoleSocketHandler.reset();
  userTerminalSocketHandler.reset();
  serverSocketHandler.reset();
  clientSocketHandler.reset();
  routerSocketHandler.reset();
  FATAL_FAIL(::remove(pipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

// TODO: Multiple clients

// TODO: FlakySocket
}  // namespace et