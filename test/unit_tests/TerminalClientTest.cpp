#include "Headers.hpp"
#include "TerminalClient.hpp"
#include "TestHeaders.hpp"

#ifndef WIN32
#include <cstdlib>
#include <fstream>
#include <functional>

#include "FakeConsole.hpp"
#include "FakeSshSetupHandler.hpp"
#include "TerminalServer.hpp"
#include "UserTerminalHandler.hpp"
#endif

using namespace et;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("refreshAgentProxyPath creates and retargets agent proxy socket",
          "[TerminalClient]") {
  const string clientId = "test-" + genRandomAlphaNum(12);
  const string target1 = "/tmp/test-agent-sock-1";
  const string target2 = "/tmp/test-agent-sock-2";

  const fs::path expectedDir =
      fs::path(GetTempDirectory()) / ("et-agent-" + clientId);

  // Clean up any stale directory beforehand
  std::error_code ec;
  fs::remove_all(expectedDir, ec);

  string proxyPath1 = refreshAgentProxyPath(clientId, target1);

#ifdef WIN32
  // On Windows, if symlink creation succeeds, proxyPath1 points to agent.sock.
  // If unprivileged/developer-mode disabled, it safely falls back to target1.
  if (proxyPath1 != target1) {
    REQUIRE_THAT(proxyPath1, ContainsSubstring(clientId));
    REQUIRE_THAT(proxyPath1, ContainsSubstring("agent.sock"));
  }
#else
  REQUIRE_THAT(proxyPath1, ContainsSubstring(clientId));
  REQUIRE_THAT(proxyPath1, ContainsSubstring("agent.sock"));
  REQUIRE(fs::is_symlink(proxyPath1));
  REQUIRE(fs::read_symlink(proxyPath1).string() == target1);

  // Verify secure directory permissions (0700)
  struct stat st {};
  REQUIRE(::stat(expectedDir.c_str(), &st) == 0);
  REQUIRE((st.st_mode & 0777) == (S_IRUSR | S_IWUSR | S_IXUSR));
#endif

  // Re-calling refreshAgentProxyPath simulates retargeting after reconnect
  // (Issue #506)
  string proxyPath2 = refreshAgentProxyPath(clientId, target2);
  REQUIRE(proxyPath1 == proxyPath2);

#ifndef WIN32
  REQUIRE(fs::is_symlink(proxyPath2));
  REQUIRE(fs::read_symlink(proxyPath2).string() == target2);
#endif

  fs::remove_all(expectedDir, ec);
}

#ifndef WIN32
namespace {

void requireEventually(const std::function<bool()>& fn, int seconds,
                       const string& what) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  FAIL("Timed out waiting for " << what);
}

string agentProxyPathFor(const string& clientId) {
  return (fs::path(GetTempDirectory()) / ("et-agent-" + clientId) /
          "agent.sock")
      .string();
}

string readAgentProxyTarget(const string& clientId) {
  return fs::read_symlink(agentProxyPathFor(clientId)).string();
}

struct EnvVarGuard {
  EnvVarGuard(const char* name, const string& value) : name_(name) {
    const char* previous = ::getenv(name);
    if (previous) {
      previous_ = previous;
      hadPrevious_ = true;
    }
    REQUIRE(::setenv(name, value.c_str(), 1) == 0);
  }
  ~EnvVarGuard() {
    if (hadPrevious_) {
      ::setenv(name_, previous_.c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }
  const char* name_;
  string previous_;
  bool hadPrevious_ = false;
};

// Subclass so tests can force the in-process reconnect path that keeps the
// server-side reverse-tunnel destination from the first InitialPayload.
struct TestTerminalClient : public TerminalClient {
  using TerminalClient::TerminalClient;

  void forceReconnect() { connection->closeSocketAndMaybeReconnect(); }
  bool isDisconnected() { return connection->isDisconnected(); }
  int socketFd() { return connection->getSocketFd(); }
};

struct AgentProxySession {
  string pipeDirectory;
  shared_ptr<PipeSocketHandler> serverSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;
  shared_ptr<PipeSocketHandler> consoleSocketHandler;
  shared_ptr<PipeSocketHandler> clientSocketHandler;
  shared_ptr<PipeSocketHandler> clientPipeSocketHandler;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;
  shared_ptr<FakeConsole> console;
  shared_ptr<TerminalServer> server;
  shared_ptr<UserTerminalHandler> uth;
  shared_ptr<TestTerminalClient> client;
  thread serverThread;
  thread uthThread;
  thread clientThread;
  string id;
  string passkey;
  SocketEndpoint serverEndpoint;
  SocketEndpoint routerEndpoint;

  AgentProxySession() {
    serverSocketHandler = make_shared<PipeSocketHandler>();
    routerSocketHandler = make_shared<PipeSocketHandler>();
    consoleSocketHandler = make_shared<PipeSocketHandler>();
    clientSocketHandler = make_shared<PipeSocketHandler>();
    clientPipeSocketHandler = make_shared<PipeSocketHandler>();

    pipeDirectory = test::makeTempDir("et_agent_proxy");
    routerEndpoint.set_name(pipeDirectory + "/router");
    serverEndpoint.set_name(pipeDirectory + "/server");

    server = make_shared<TerminalServer>(serverSocketHandler, serverEndpoint,
                                         routerSocketHandler, routerEndpoint);
    serverThread = thread([this]() { server->run(); });
    testSleepMicros(200000);

    auto fakeSubprocessUtils = make_shared<FakeSubprocessUtils>();
    auto sshSetupHandler =
        make_shared<FakeSshSetupHandler>(fakeSubprocessUtils);
    auto idpass = sshSetupHandler->SetupSsh("", "localhost", "localhost", 2022,
                                            "", "", false, 0, "", "", {});
    id = idpass.first;
    passkey = idpass.second;

    fakeUserTerminal = make_shared<FakeUserTerminal>(routerSocketHandler);
    uth = make_shared<UserTerminalHandler>(routerSocketHandler,
                                           fakeUserTerminal, true,
                                           routerEndpoint, id + "/" + passkey);
    uthThread = thread([this]() { uth->run(); });
  }

  void startClient(bool forwardSshAgent, bool resumeSavedSession,
                   const string& identityAgent = "") {
    console = make_shared<FakeConsole>(consoleSocketHandler);
    client = make_shared<TestTerminalClient>(
        clientSocketHandler, clientPipeSocketHandler, serverEndpoint, id,
        passkey, console, false, "", "", forwardSshAgent, identityAgent,
        MAX_CLIENT_KEEP_ALIVE_DURATION, vector<pair<string, string>>(),
        /*noPty=*/false, /*command=*/"", vector<string>(),
        /*stdioForward=*/"", /*maxConnectAttempts=*/3, resumeSavedSession);
    clientThread = thread([this]() { client->run("", false); });
    requireEventually([this]() { return console->isSetup(); }, 30,
                      "console setup");
    requireEventually([this]() { return fakeUserTerminal->isSetup(); }, 30,
                      "user terminal setup");
  }

  void stopClient() {
    if (client) {
      client->shutdown();
      if (clientThread.joinable()) {
        clientThread.join();
      }
      client.reset();
    }
    console.reset();
  }

  ~AgentProxySession() {
    try {
      stopClient();
      if (uth) {
        uth->shutdown();
      }
      if (uthThread.joinable()) {
        uthThread.join();
      }
      uth.reset();
      if (server) {
        // Close fds only after run() stops selecting on them.
        server->shutdown();
        if (serverThread.joinable()) {
          serverThread.join();
        }
        server->shutdownConnections();
        server.reset();
      } else if (serverThread.joinable()) {
        serverThread.join();
      }
      serverSocketHandler.reset();
      routerSocketHandler.reset();
      fakeUserTerminal.reset();
      if (!pipeDirectory.empty()) {
        ::remove((pipeDirectory + "/router").c_str());
        ::remove((pipeDirectory + "/server").c_str());
        test::removeTempDir(pipeDirectory);
      }
      std::error_code ec;
      if (!id.empty()) {
        fs::remove_all(fs::path(GetTempDirectory()) / ("et-agent-" + id), ec);
      }
    } catch (...) {
    }
  }
};

}  // namespace

TEST_CASE("TerminalClient reattach retargets SSH agent proxy",
          "[TerminalClient]") {
  const string sock1 =
      (fs::path(test::makeTempDir("et_agent_sock")) / "agent1.sock").string();
  const string sock2 =
      (fs::path(fs::path(sock1).parent_path()) / "agent2.sock").string();
  // Touch placeholder socket paths so symlink targets exist for debugging.
  { std::ofstream(sock1).close(); }
  { std::ofstream(sock2).close(); }

  AgentProxySession session;
  {
    EnvVarGuard authSock("SSH_AUTH_SOCK", sock1);
    session.startClient(/*forwardSshAgent=*/true,
                        /*resumeSavedSession=*/false);
    REQUIRE(readAgentProxyTarget(session.id) == sock1);
    session.stopClient();
  }

  // Reattach mirrors saved-session attach: forwardSshAgent is false, but the
  // server still has the original reverse-tunnel destination.
  {
    EnvVarGuard authSock("SSH_AUTH_SOCK", sock2);
    session.startClient(/*forwardSshAgent=*/false,
                        /*resumeSavedSession=*/true);
    REQUIRE(readAgentProxyTarget(session.id) == sock2);
  }

  std::error_code ec;
  fs::remove_all(fs::path(sock1).parent_path(), ec);
}

TEST_CASE("TerminalClient in-process reconnect retargets SSH agent proxy",
          "[TerminalClient]") {
  const string sockDir = test::makeTempDir("et_agent_sock");
  const string sock1 = (fs::path(sockDir) / "agent1.sock").string();
  const string sock2 = (fs::path(sockDir) / "agent2.sock").string();
  { std::ofstream(sock1).close(); }
  { std::ofstream(sock2).close(); }

  AgentProxySession session;
  EnvVarGuard authSock("SSH_AUTH_SOCK", sock1);
  session.startClient(/*forwardSshAgent=*/true, /*resumeSavedSession=*/false);
  REQUIRE(readAgentProxyTarget(session.id) == sock1);

  REQUIRE(::setenv("SSH_AUTH_SOCK", sock2.c_str(), 1) == 0);
  session.client->forceReconnect();
  requireEventually([&]() { return !session.client->isDisconnected(); }, 30,
                    "client reconnect");
  requireEventually([&]() { return readAgentProxyTarget(session.id) == sock2; },
                    10, "agent proxy retarget after reconnect");

  std::error_code ec;
  fs::remove_all(sockDir, ec);
}
#endif
