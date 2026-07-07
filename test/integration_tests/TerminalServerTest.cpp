#include <atomic>

#include "FakeConsole.hpp"
#include "FakeSshSetupHandler.hpp"
#include "SshSetupHandler.hpp"
#include "SubprocessUtils.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"
#include "TunnelUtils.hpp"
#include "UserJumphostHandler.hpp"
#include "UserTerminalHandler.hpp"

namespace et {

class ServerTestFakeSshSetupHandler : public FakeSshSetupHandler {
 public:
  explicit ServerTestFakeSshSetupHandler(
      shared_ptr<SubprocessUtils> subprocessUtils,
      shared_ptr<PipeSocketHandler> _serverSocketHandler,
      shared_ptr<PipeSocketHandler> _routerSocketHandler,
      shared_ptr<PipeSocketHandler> _jumphostRouterSocketHandler,
      map<pair<string, int>, vector<shared_ptr<FakeUserTerminal>>>
          _fakeUserTerminals,
      const SocketEndpoint& _routerEndpoint,
      const SocketEndpoint& _serverEndpoint,
      const SocketEndpoint& _jumphostRouterEndpoint)
      : FakeSshSetupHandler(subprocessUtils),
        serverSocketHandler(_serverSocketHandler),
        routerSocketHandler(_routerSocketHandler),
        jumphostRouterSocketHandler(_jumphostRouterSocketHandler),
        fakeUserTerminals(_fakeUserTerminals),
        routerEndpoint(_routerEndpoint),
        serverEndpoint(_serverEndpoint),
        jumphostRouterEndpoint(_jumphostRouterEndpoint) {}

  ~ServerTestFakeSshSetupHandler() { shutdownHandler(); }

  pair<string, string> SetupSsh(
      const string& user, const string& host, const string& host_alias,
      int port, const string& jumphost, const string& jServerFifo, bool kill,
      int vlevel, const string& etterminal_path, const string& serverFifo,
      const std::vector<std::string>& ssh_options) override {
    auto [id, passkey] = FakeSshSetupHandler::SetupSsh(
        user, host, host_alias, port, jumphost, jServerFifo, kill, vlevel,
        etterminal_path, serverFifo, ssh_options);

    if (jumphost.empty()) {
      // Get the FakeUserTerminal for this host:port combination
      auto key = make_pair(host, port);
      auto it = fakeUserTerminals.find(key);
      if (it == fakeUserTerminals.end() || it->second.empty()) {
        throw runtime_error("No FakeUserTerminal available for " + host + ":" +
                            to_string(port));
      }

      // Get the first FakeUserTerminal from the vector
      shared_ptr<FakeUserTerminal> fakeUserTerminal = it->second.front();
      // Remove it from the vector so the next call gets the next element
      it->second.erase(it->second.begin());

      // Create and maintain the UserTerminalHandler
      shared_ptr<UserTerminalHandler> userTerminalHandler(
          new UserTerminalHandler(routerSocketHandler, fakeUserTerminal, true,
                                  routerEndpoint, id + "/" + passkey));
      userTerminalHandlers.push_back(userTerminalHandler);
      handlerThreads.push_back(
          thread([userTerminalHandler]() { userTerminalHandler->run(); }));
    } else {
      // Get the FakeUserTerminal for this host:port combination
      auto key = make_pair(host, port);
      auto it = fakeUserTerminals.find(key);
      if (it == fakeUserTerminals.end() || it->second.empty()) {
        throw runtime_error("No FakeUserTerminal available for " + host + ":" +
                            to_string(port));
      }

      // Get the first FakeUserTerminal from the vector
      shared_ptr<FakeUserTerminal> fakeUserTerminal = it->second.front();
      // Remove it from the vector so the next call gets the next element
      it->second.erase(it->second.begin());

      // Create and maintain the UserTerminalHandler
      shared_ptr<UserTerminalHandler> userTerminalHandler(
          new UserTerminalHandler(routerSocketHandler, fakeUserTerminal, true,
                                  routerEndpoint, id + "/" + passkey));
      userTerminalHandlers.push_back(userTerminalHandler);
      handlerThreads.push_back(
          thread([userTerminalHandler]() { userTerminalHandler->run(); }));
      auto tokens = split(jumphost, ':');
      string jhost = tokens[0];
      int jport = stoi(tokens[1]);

      // Create a socket handler for the jumphost to connect to the destination
      shared_ptr<PipeSocketHandler> jumphostClientSocketHandler(
          new PipeSocketHandler());

      // Create and maintain the UserJumphostHandler
      shared_ptr<UserJumphostHandler> userJumphostHandler(
          new UserJumphostHandler(
              jumphostClientSocketHandler, id + "/" + passkey, serverEndpoint,
              jumphostRouterSocketHandler, jumphostRouterEndpoint));
      userJumphostHandlers.push_back(userJumphostHandler);
      handlerThreads.push_back(
          thread([userJumphostHandler]() { userJumphostHandler->run(); }));
    }

    return {id, passkey};
  }

  void shutdownHandler() {
    for (auto& handler : userTerminalHandlers) {
      if (handler) {
        handler->shutdown();
      }
    }
    for (auto& thread : handlerThreads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    userTerminalHandlers.clear();
    handlerThreads.clear();
  }

  void addFakeUserTerminal(const string& host, int port,
                           shared_ptr<FakeUserTerminal> terminal) {
    auto key = make_pair(host, port);
    fakeUserTerminals[key].push_back(terminal);
  }

 private:
  shared_ptr<PipeSocketHandler> serverSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;
  shared_ptr<PipeSocketHandler> jumphostRouterSocketHandler;
  map<pair<string, int>, vector<shared_ptr<FakeUserTerminal>>>
      fakeUserTerminals;
  SocketEndpoint routerEndpoint;
  SocketEndpoint serverEndpoint;
  vector<shared_ptr<UserTerminalHandler>> userTerminalHandlers;
  vector<shared_ptr<UserJumphostHandler>> userJumphostHandlers;
  SocketEndpoint jumphostRouterEndpoint;
  vector<thread> handlerThreads;
};

void serverReadWriteTest(
    shared_ptr<ServerTestFakeSshSetupHandler> sshSetupHandler,
    shared_ptr<FakeUserTerminal> fakeUserTerminal,
    SocketEndpoint serverEndpoint,
    shared_ptr<SocketHandler> clientSocketHandler,
    shared_ptr<SocketHandler> clientPipeSocketHandler,
    shared_ptr<FakeConsole> fakeConsole, shared_ptr<TerminalServer> server) {
  // Get id/passkey and create UserTerminalHandler through the
  // FakeSshSetupHandler
  auto [id, passkey] = sshSetupHandler->SetupSsh(
      "", "localhost", "localhost", 2022, "", "", false, 0, "", "", {});

  sleep(1);

  shared_ptr<TerminalClient> terminalClient(
      new TerminalClient(clientSocketHandler, clientPipeSocketHandler,
                         serverEndpoint, id, passkey, fakeConsole, false, "",
                         "", false, "", MAX_CLIENT_KEEP_ALIVE_DURATION, {}));
  thread terminalClientThread(
      [terminalClient]() { terminalClient->run("", false); });
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

  sshSetupHandler->shutdownHandler();
}

class ServerEndToEndTestFixture {
 public:
  ServerEndToEndTestFixture() {
    srand(1);
    clientSocketHandler.reset(new PipeSocketHandler());
    clientPipeSocketHandler.reset(new PipeSocketHandler());
    serverSocketHandler.reset(new PipeSocketHandler());
    routerSocketHandler.reset(new PipeSocketHandler());
    el::Helpers::setThreadName("Main");
    consoleSocketHandler.reset(new PipeSocketHandler());
    fakeConsole.reset(new FakeConsole(consoleSocketHandler));
    jumphostServerSocketHandler.reset(new PipeSocketHandler());
    jumphostRouterSocketHandler.reset(new PipeSocketHandler());

    userTerminalSocketHandler.reset(new PipeSocketHandler());
    fakeUserTerminal.reset(new FakeUserTerminal(userTerminalSocketHandler));

    string tmpPath = GetTempDirectory() + string("etserver_test_XXXXXXXX");
    pipeDirectory = string(mkdtemp(&tmpPath[0]));

    routerPipePath = string(pipeDirectory) + "/pipe_router";
    routerEndpoint.set_name(routerPipePath);

    serverPipePath = string(pipeDirectory) + "/pipe_server";
    serverEndpoint.set_name(serverPipePath);

    // Create the TerminalServer
    server = shared_ptr<TerminalServer>(
        new TerminalServer(serverSocketHandler, serverEndpoint,
                           routerSocketHandler, routerEndpoint));
    serverThread = thread([this]() { server->run(); });
    sleep(1);

    // Create the FakeSshSetupHandler that will manage UserTerminalHandler
    auto fakeSubprocessUtils = make_shared<FakeSubprocessUtils>();

    // Create a map for the fake user terminals
    map<pair<string, int>, vector<shared_ptr<FakeUserTerminal>>>
        fakeUserTerminalsMap;
    fakeUserTerminalsMap[make_pair("localhost", 2022)].push_back(
        fakeUserTerminal);

    // Create the TerminalServer for the jumphost
    jumphostRouterPipePath = string(pipeDirectory) + "/jumphost_pipe_router";
    jumphostRouterEndpoint.set_name(jumphostRouterPipePath);

    jumphostServerPipePath = string(pipeDirectory) + "/jumphost_pipe_server";
    jumphostServerEndpoint.set_name(jumphostServerPipePath);
    jumphostServer = shared_ptr<TerminalServer>(new TerminalServer(
        jumphostServerSocketHandler, jumphostServerEndpoint,
        jumphostRouterSocketHandler, jumphostRouterEndpoint));
    jumphostServerThread = thread([this]() { jumphostServer->run(); });
    sleep(1);

    sshSetupHandler = make_shared<ServerTestFakeSshSetupHandler>(
        fakeSubprocessUtils, serverSocketHandler, routerSocketHandler,
        jumphostRouterSocketHandler, fakeUserTerminalsMap, routerEndpoint,
        serverEndpoint, jumphostRouterEndpoint);
  }

  ~ServerEndToEndTestFixture() {
    sshSetupHandler.reset();

    server->shutdown();
    serverThread.join();
    server.reset();

    jumphostServer->shutdown();
    jumphostServerThread.join();
    jumphostServer.reset();

    consoleSocketHandler.reset();
    userTerminalSocketHandler.reset();
    serverSocketHandler.reset();
    clientSocketHandler.reset();
    clientPipeSocketHandler.reset();
    routerSocketHandler.reset();
    FATAL_FAIL(::remove(routerPipePath.c_str()));
    FATAL_FAIL(::remove(serverPipePath.c_str()));
    FATAL_FAIL(::remove(jumphostRouterPipePath.c_str()));
    FATAL_FAIL(::remove(jumphostServerPipePath.c_str()));
    FATAL_FAIL(::remove(pipeDirectory.c_str()));
  }

 protected:
  shared_ptr<PipeSocketHandler> consoleSocketHandler;
  shared_ptr<PipeSocketHandler> userTerminalSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;
  shared_ptr<PipeSocketHandler> jumphostServerSocketHandler;
  shared_ptr<PipeSocketHandler> jumphostRouterSocketHandler;

  shared_ptr<PipeSocketHandler> serverSocketHandler;
  shared_ptr<PipeSocketHandler> clientSocketHandler;
  shared_ptr<PipeSocketHandler> clientPipeSocketHandler;

  string pipeDirectory;

  SocketEndpoint serverEndpoint;
  string serverPipePath;

  SocketEndpoint routerEndpoint;
  string routerPipePath;

  SocketEndpoint jumphostRouterEndpoint;
  string jumphostRouterPipePath;

  SocketEndpoint jumphostServerEndpoint;
  string jumphostServerPipePath;

  shared_ptr<FakeConsole> fakeConsole;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;

  shared_ptr<TerminalServer> server;
  thread serverThread;

  shared_ptr<TerminalServer> jumphostServer;
  thread jumphostServerThread;

  shared_ptr<ServerTestFakeSshSetupHandler> sshSetupHandler;
};

TEST_CASE_METHOD(ServerEndToEndTestFixture, "ServerEndToEndTest",
                 "[ServerEndToEndTest][integration]") {
  serverReadWriteTest(sshSetupHandler, fakeUserTerminal, serverEndpoint,
                      clientSocketHandler, clientPipeSocketHandler, fakeConsole,
                      server);
}

TEST_CASE_METHOD(ServerEndToEndTestFixture, "ServerMultipleClientsTest",
                 "[ServerMultipleClientsTest][integration]") {
  const int numClients = 3;

  // Create arrays for multiple clients
  vector<shared_ptr<PipeSocketHandler>> clientConsoleSocketHandlers;
  vector<shared_ptr<FakeConsole>> fakeConsoles;
  vector<shared_ptr<PipeSocketHandler>> clientUserTerminalSocketHandlers;
  vector<shared_ptr<FakeUserTerminal>> fakeUserTerminals;
  vector<shared_ptr<SocketHandler>> clientSocketHandlers;
  vector<shared_ptr<SocketHandler>> clientPipeSocketHandlers;

  // Use the fixture's fakeUserTerminal for the first client
  fakeUserTerminals.push_back(fakeUserTerminal);
  fakeConsoles.push_back(fakeConsole);
  clientSocketHandlers.push_back(clientSocketHandler);
  clientPipeSocketHandlers.push_back(clientPipeSocketHandler);

  // Create additional fake consoles and user terminals for remaining clients
  for (int i = 1; i < numClients; i++) {
    auto consoleHandler = make_shared<PipeSocketHandler>();
    clientConsoleSocketHandlers.push_back(consoleHandler);
    fakeConsoles.push_back(make_shared<FakeConsole>(consoleHandler));

    auto userTerminalHandler = make_shared<PipeSocketHandler>();
    clientUserTerminalSocketHandlers.push_back(userTerminalHandler);
    auto userTerminal = make_shared<FakeUserTerminal>(userTerminalHandler);
    fakeUserTerminals.push_back(userTerminal);

    clientSocketHandlers.push_back(make_shared<PipeSocketHandler>());
    clientPipeSocketHandlers.push_back(make_shared<PipeSocketHandler>());

    // Add the new terminal to the ssh setup handler
    sshSetupHandler->addFakeUserTerminal("localhost", 2022, userTerminal);
  }

  // Arrays for client state
  vector<string> ids;
  vector<string> passkeys;
  vector<shared_ptr<TerminalClient>> terminalClients;
  vector<thread> terminalClientThreads;

  // Connect all clients
  for (int i = 0; i < numClients; i++) {
    auto [id, passkey] = sshSetupHandler->SetupSsh(
        "", "localhost", "localhost", 2022, "", "", false, 0, "", "", {});
    ids.push_back(id);
    passkeys.push_back(passkey);

    sleep(2);

    auto client = make_shared<TerminalClient>(
        clientSocketHandlers[i], clientPipeSocketHandlers[i], serverEndpoint,
        id, passkey, fakeConsoles[i], false, "", "", false, "",
        MAX_CLIENT_KEEP_ALIVE_DURATION, vector<pair<string, string>>());
    terminalClients.push_back(client);
    terminalClientThreads.push_back(
        thread([client]() { client->run("", false); }));
  }

  // Wait for all connections to be fully established
  sleep(5);

  // Create unique strings for each client
  vector<string> uniqueStrings = {"client_0_data", "client_1_data",
                                  "client_2_data"};

  // Send data from all clients concurrently
  vector<thread> sendThreads;
  for (int i = 0; i < numClients; i++) {
    sendThreads.push_back(thread([i, &fakeConsoles, &uniqueStrings]() {
      const string& s = uniqueStrings[i];
      for (size_t a = 0; a < s.size(); a++) {
        fakeConsoles[i]->simulateKeystrokes(string(1, s[a]));
      }
    }));
  }

  // Receive and verify data from all clients concurrently
  vector<thread> receiveThreads;
  vector<string> results(numClients);
  for (int i = 0; i < numClients; i++) {
    receiveThreads.push_back(
        thread([i, &fakeUserTerminals, &uniqueStrings, &results]() {
          const string& expected = uniqueStrings[i];
          string result;
          for (size_t a = 0; a < expected.size(); a++) {
            string r = fakeUserTerminals[i]->getKeystrokes(1);
            result = result.append(r);
          }
          results[i] = result;
        }));
  }

  // Wait for all sends and receives
  for (auto& t : sendThreads) {
    t.join();
  }
  for (auto& t : receiveThreads) {
    t.join();
  }

  // Verify all clients received the correct data
  for (int i = 0; i < numClients; i++) {
    REQUIRE(results[i] == uniqueStrings[i]);
  }

  // Cleanup
  for (auto& client : terminalClients) {
    client->shutdown();
  }
  for (auto& thread : terminalClientThreads) {
    thread.join();
  }
  terminalClients.clear();

  sshSetupHandler->shutdownHandler();
}

TEST_CASE_METHOD(ServerEndToEndTestFixture, "ServerDataTransferTest",
                 "[ServerDataTransferTest][integration]") {
  // Get credentials for the session
  auto [id, passkey] = sshSetupHandler->SetupSsh(
      "", "localhost", "localhost", 2022, "", "", false, 0, "", "", {});

  sleep(1);

  shared_ptr<TerminalClient> terminalClient(
      new TerminalClient(clientSocketHandler, clientPipeSocketHandler,
                         serverEndpoint, id, passkey, fakeConsole, false, "",
                         "", false, "", MAX_CLIENT_KEEP_ALIVE_DURATION, {}));
  thread terminalClientThread(
      [terminalClient]() { terminalClient->run("", false); });
  sleep(3);

  // Test sending smaller amount of data
  string s = "test_data_transfer";
  for (int a = 0; a < s.size(); a++) {
    fakeConsole->simulateKeystrokes(string(1, s[a]));
  }

  string resultConcat;
  for (int a = 0; a < s.size(); a++) {
    string result = fakeUserTerminal->getKeystrokes(1);
    resultConcat = resultConcat.append(result);
  }

  REQUIRE(resultConcat == s);

  terminalClient->shutdown();
  terminalClientThread.join();
  terminalClient.reset();

  sshSetupHandler->shutdownHandler();
}

TEST_CASE_METHOD(ServerEndToEndTestFixture, "ServerJumphostTest",
                 "[ServerJumphostTest][integration]") {
  // The destination terminal is already set up in the fixture at
  // "localhost:2022"

  // Get id/passkey and create UserTerminalHandler and UserJumphostHandler
  // through the FakeSshSetupHandler with jumphost parameter
  auto [id, passkey] =
      sshSetupHandler->SetupSsh("", "localhost", "localhost", 2022,
                                "jumphost:2023", "", false, 0, "", "", {});

  sleep(1);

  shared_ptr<TerminalClient> terminalClient(new TerminalClient(
      clientSocketHandler, clientPipeSocketHandler, jumphostServerEndpoint, id,
      passkey, fakeConsole, true, "", "", false, "",
      MAX_CLIENT_KEEP_ALIVE_DURATION, {}));
  thread terminalClientThread(
      [terminalClient]() { terminalClient->run("", false); });
  sleep(3);

  // Create a test string to send through the jumphost
  string testData = "jumphost_test_data";

  // Send the data from the client console
  thread sendThread([this, testData]() {
    for (size_t i = 0; i < testData.size(); i++) {
      fakeConsole->simulateKeystrokes(string(1, testData[i]));
    }
  });

  // Receive the data on the destination terminal (not the jumphost terminal)
  string result;
  for (size_t i = 0; i < testData.size(); i++) {
    string r = fakeUserTerminal->getKeystrokes(1);
    result.append(r);
    LOG(INFO) << "Received character " << i << ": " << r;
  }

  sendThread.join();

  // Verify the data was received correctly at the destination
  REQUIRE(result == testData);

  terminalClient->shutdown();
  terminalClientThread.join();
  terminalClient.reset();

  sshSetupHandler->shutdownHandler();
}

}  // namespace et
