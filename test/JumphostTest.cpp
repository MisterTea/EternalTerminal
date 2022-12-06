#include "FakeConsole.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"
#include "UserJumphostHandler.hpp"

namespace et {

const string CRYPTO_KEY = "12345678901234567890123456789012";

void readWriteTest(const string& clientId,
                   shared_ptr<PipeSocketHandler> routerSocketHandler,
                   shared_ptr<FakeUserTerminal> fakeUserTerminal,
                   SocketEndpoint serverEndpoint,
                   shared_ptr<SocketHandler> clientSocketHandler,
                   shared_ptr<SocketHandler> clientPipeSocketHandler,
                   shared_ptr<FakeConsole> fakeConsole,
                   const SocketEndpoint& routerEndpoint,
                   shared_ptr<PipeSocketHandler> jumphostUserSocketHandler,
                   shared_ptr<PipeSocketHandler> jumphostRouterSocketHandler,
                   const SocketEndpoint& jumphostRouterEndpoint,
                   const SocketEndpoint& jumphostEndpoint) {
  auto ujh = shared_ptr<UserJumphostHandler>(new UserJumphostHandler(
      jumphostUserSocketHandler, clientId + "/" + CRYPTO_KEY, serverEndpoint,
      jumphostRouterSocketHandler, jumphostRouterEndpoint));
  thread ujhThread([ujh]() { ujh->run(); });
  sleep(3);

  auto uth = shared_ptr<UserTerminalHandler>(
      new UserTerminalHandler(routerSocketHandler, fakeUserTerminal, true,
                              routerEndpoint, clientId + "/" + CRYPTO_KEY));
  thread uthThread([uth]() { uth->run(); });
  sleep(3);

  shared_ptr<TerminalClient> terminalClient(new TerminalClient(
      clientSocketHandler, clientPipeSocketHandler, jumphostEndpoint, clientId,
      CRYPTO_KEY, fakeConsole, true, "", "", false, "",
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
  sleep(1);

  terminalClient->shutdown();
  terminalClientThread.join();
  terminalClient.reset();

  uth->shutdown();
  uthThread.join();
  uth.reset();

  ujh->shutdown();
  ujhThread.join();
  ujh.reset();
}

TEST_CASE("JumphostEndToEndTest", "[JumphostEndToEndTest]") {
  shared_ptr<PipeSocketHandler> consoleSocketHandler(new PipeSocketHandler());
  shared_ptr<PipeSocketHandler> terminalUserSocketHandler(
      new PipeSocketHandler());
  shared_ptr<PipeSocketHandler> routerSocketHandler(new PipeSocketHandler());
  shared_ptr<PipeSocketHandler> serverSocketHandler(new PipeSocketHandler());
  SocketEndpoint serverEndpoint;
  shared_ptr<FakeConsole> fakeConsole;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;

  shared_ptr<PipeSocketHandler> jumphostUserSocketHandler(
      new PipeSocketHandler());
  shared_ptr<PipeSocketHandler> jumphostRouterSocketHandler(
      new PipeSocketHandler());
  shared_ptr<PipeSocketHandler> jumphostSocketHandler(new PipeSocketHandler());
  SocketEndpoint jumphostEndpoint;

  shared_ptr<PipeSocketHandler> clientSocketHandler(new PipeSocketHandler());
  shared_ptr<PipeSocketHandler> clientPipeSocketHandler(
      new PipeSocketHandler());

  string pipeDirectory;

  srand(1);
  el::Helpers::setThreadName("Main");
  fakeConsole.reset(new FakeConsole(consoleSocketHandler));

  fakeUserTerminal.reset(new FakeUserTerminal(terminalUserSocketHandler));
  fakeUserTerminal->setup(-1);

  string tmpPath = GetTempDirectory() + string("etserver_test_XXXXXXXX");
  pipeDirectory = string(mkdtemp(&tmpPath[0]));

  string routerPipePath = string(pipeDirectory) + "/pipe_router";
  SocketEndpoint routerEndpoint;
  routerEndpoint.set_name(routerPipePath);

  string serverPipePath = string(pipeDirectory) + "/pipe_server";
  serverEndpoint.set_name(serverPipePath);

  string jumphostRouterPipePath =
      string(pipeDirectory) + "/pipe_jumphost_router";
  SocketEndpoint jumphostRouterEndpoint;
  jumphostRouterEndpoint.set_name(jumphostRouterPipePath);

  string jumphostServerPipePath =
      string(pipeDirectory) + "/pipe_jumphost_server";
  jumphostEndpoint.set_name(jumphostServerPipePath);

  auto server = shared_ptr<TerminalServer>(
      new TerminalServer(serverSocketHandler, serverEndpoint,
                         routerSocketHandler, routerEndpoint));
  thread t_server([server]() { server->run(); });
  sleep(3);

  auto jumphost = shared_ptr<TerminalServer>(
      new TerminalServer(jumphostSocketHandler, jumphostEndpoint,
                         jumphostRouterSocketHandler, jumphostRouterEndpoint));
  thread t_jumphost([jumphost]() { jumphost->run(); });
  sleep(3);

  readWriteTest("1234567890123456", routerSocketHandler, fakeUserTerminal,
                serverEndpoint, clientSocketHandler, clientPipeSocketHandler,
                fakeConsole, routerEndpoint, jumphostUserSocketHandler,
                jumphostRouterSocketHandler, jumphostRouterEndpoint,
                jumphostEndpoint);
  server->shutdown();
  t_server.join();

  consoleSocketHandler.reset();
  terminalUserSocketHandler.reset();
  serverSocketHandler.reset();

  jumphost->shutdown();
  t_jumphost.join();

  jumphostUserSocketHandler.reset();
  jumphostRouterSocketHandler.reset();
  jumphostSocketHandler.reset();

  clientSocketHandler.reset();
  clientPipeSocketHandler.reset();
  routerSocketHandler.reset();

  FATAL_FAIL(::remove(jumphostRouterPipePath.c_str()));
  FATAL_FAIL(::remove(jumphostServerPipePath.c_str()));
  FATAL_FAIL(::remove(routerPipePath.c_str()));
  FATAL_FAIL(::remove(serverPipePath.c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}
}  // namespace et
