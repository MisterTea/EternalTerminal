#include "TestHeaders.hpp"

#include "FakeConsole.hpp"
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
      CRYPTO_KEY, fakeConsole, false, "", "", false, ""));
  thread terminalClientThread(
      [terminalClient]() { terminalClient->run(""); });
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

TEST_CASE("EndToEndTest", "[EndToEndTest]") {
  shared_ptr<PipeSocketHandler> consoleSocketHandler;
  shared_ptr<PipeSocketHandler> userTerminalSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;

  shared_ptr<SocketHandler> serverSocketHandler;
  shared_ptr<SocketHandler> clientSocketHandler;
  shared_ptr<SocketHandler> clientPipeSocketHandler;

  SocketEndpoint serverEndpoint;

  shared_ptr<FakeConsole> fakeConsole;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;

  string pipeDirectory;

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
  fakeUserTerminal->setup(-1);

  string tmpPath = GetTempDirectory() + string("etserver_test_XXXXXXXX");
  pipeDirectory = string(mkdtemp(&tmpPath[0]));

  string routerPipePath = string(pipeDirectory) + "/pipe_router";
  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(routerPipePath);

  string serverPipePath = string(pipeDirectory) + "/pipe_server";
  serverEndpoint.set_name(serverPipePath);

  auto server = shared_ptr<TerminalServer>(
      new TerminalServer(serverSocketHandler, serverEndpoint,
                         routerSocketHandler, routerEndpoint));
  thread t_server([server]() { server->run(); });
  sleep(1);

  readWriteTest("1234567890123456", routerSocketHandler, fakeUserTerminal,
                serverEndpoint, clientSocketHandler, clientPipeSocketHandler, fakeConsole,
                routerEndpoint);
  server->shutdown();
  t_server.join();

  consoleSocketHandler.reset();
  userTerminalSocketHandler.reset();
  serverSocketHandler.reset();
  clientSocketHandler.reset();
  clientPipeSocketHandler.reset();
  routerSocketHandler.reset();
  FATAL_FAIL(::remove(routerPipePath.c_str()));
  FATAL_FAIL(::remove(serverPipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

// TODO: Multiple clients

// TODO: FlakySocket
}  // namespace et
