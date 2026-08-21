#ifndef WIN32
// Integration tests for etserver (router) restarts: sessions must survive.
// The "kill" is modeled in-process by shutting the TerminalServer down
// completely (listen fds, client connections, and router pipes all close, so
// remote ends observe EOF exactly as they do when the etserver process dies)
// and then binding a brand new TerminalServer on the same pipe paths.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>

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
#include "SessionStore.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"

namespace et {
namespace {

// Polls `fn` until it returns true; fails the test after `seconds`.
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

// A TerminalServer that can be killed and replaced on the same pipe paths,
// standing in for an etserver process restart.
struct RestartableServer {
  ~RestartableServer() {
    try {
      kill();
    } catch (...) {
    }
  }

  void start() {
    serverSocketHandler.reset(new PipeSocketHandler());
    routerSocketHandler.reset(new PipeSocketHandler());
    server = shared_ptr<TerminalServer>(
        new TerminalServer(serverSocketHandler, serverEndpoint,
                           routerSocketHandler, routerEndpoint));
    serverThread = thread([this]() { server->run(); });
    sleep(1);
  }

  // Simulates the etserver process dying.  Order matters: halt the accept
  // loop and let run() finish (it joins the pump threads) BEFORE closing any
  // fd, so no thread is selecting on a descriptor we close.  Only then do
  // client connections and terminal pipes see their EOF, exactly as when the
  // kernel reaps a dead process.
  void kill() {
    if (!server) {
      return;
    }
    server->shutdown();
    if (serverThread.joinable()) {
      serverThread.join();
    }
    server->shutdownConnections();
    server.reset();
    serverSocketHandler.reset();
    routerSocketHandler.reset();
  }

  shared_ptr<TerminalServer> server;
  thread serverThread;
  shared_ptr<SocketHandler> serverSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;
  SocketEndpoint serverEndpoint;
  SocketEndpoint routerEndpoint;
};

// One client+etterminal session pair.
struct SessionFixture {
  ~SessionFixture() {
    // A failing REQUIRE unwinds through this dtor; destroying a joinable
    // thread would terminate the process and mask the real failure.
    try {
      stop();
    } catch (...) {
    }
  }

  void start(RestartableServer& target,
             std::function<bool(const string&)> sessionTitleUpdate = {}) {
    auto fakeSubprocessUtils = make_shared<FakeSubprocessUtils>();
    auto sshSetupHandler =
        make_shared<FakeSshSetupHandler>(fakeSubprocessUtils);
    auto idpass = sshSetupHandler->SetupSsh("", "localhost", "localhost", 2022,
                                            "", "", false, 0, "", "", {});
    id = idpass.first;
    passkey = idpass.second;

    consoleSocketHandler.reset(new PipeSocketHandler());
    userTerminalSocketHandler.reset(new PipeSocketHandler());
    console.reset(new FakeConsole(consoleSocketHandler));
    userTerminal.reset(new FakeUserTerminal(userTerminalSocketHandler));

    handler = shared_ptr<UserTerminalHandler>(
        new UserTerminalHandler(userTerminalSocketHandler, userTerminal, true,
                                target.routerEndpoint, id + "/" + passkey));
    handlerThread = thread([this]() { handler->run(); });

    clientSocketHandler.reset(new PipeSocketHandler());
    clientPipeSocketHandler.reset(new PipeSocketHandler());
    client = shared_ptr<TerminalClient>(new TerminalClient(
        clientSocketHandler, clientPipeSocketHandler, target.serverEndpoint, id,
        passkey, console, false, "", "", false, "",
        MAX_CLIENT_KEEP_ALIVE_DURATION, vector<pair<string, string>>(),
        /*maxConnectAttempts=*/3, /*exitOnConnectFailure=*/true,
        /*sessionHeartbeat=*/{}, sessionTitleUpdate));
    clientThread = thread([this]() { client->run("", false); });

    requireEventually([this]() { return console->isSetup(); }, 30,
                      "console setup");
    // The terminal pipe is only wired up after the client bootstrap delivers
    // TERMINAL_INIT and the handler runs term->setup().
    requireEventually([this]() { return userTerminal->isSetup(); }, 30,
                      "terminal setup");
  }

  void stop() {
    // Idempotent: the test body calls this explicitly and the dtor calls it
    // again on unwind.
    if (client) {
      client->shutdown();
      if (clientThread.joinable()) {
        clientThread.join();
      }
      client.reset();
    }
    if (handler) {
      handler->shutdown();
      if (handlerThread.joinable()) {
        handlerThread.join();
      }
      handler.reset();
    }
  }

  string id;
  string passkey;
  shared_ptr<PipeSocketHandler> consoleSocketHandler;
  shared_ptr<PipeSocketHandler> userTerminalSocketHandler;
  shared_ptr<SocketHandler> clientSocketHandler;
  shared_ptr<SocketHandler> clientPipeSocketHandler;
  shared_ptr<FakeConsole> console;
  shared_ptr<FakeUserTerminal> userTerminal;
  shared_ptr<UserTerminalHandler> handler;
  shared_ptr<TerminalClient> client;
  thread handlerThread;
  thread clientThread;
};

// A UserTerminal backed by a real pty running `cat` (raw, no echo), so the
// restart tests also cover a genuine forked child process.
class RealPtyCatTerminal : public UserTerminal {
 public:
  RealPtyCatTerminal() : masterFd(-1), childPid(-1) {}
  virtual ~RealPtyCatTerminal() {}

  virtual int setup(int routerFd) {
    struct termios tios;
    memset(&tios, 0, sizeof(tios));
    cfmakeraw(&tios);
    tios.c_cc[VMIN] = 1;
    tios.c_cc[VTIME] = 0;
    const pid_t pid = forkpty(&masterFd, NULL, &tios, NULL);
    childPid.store(pid);
    if (pid == -1) {
      FATAL_FAIL(pid);
    }
    if (pid == 0) {
      // The child must not hold copies of the server/router sockets (the
      // whole suite runs in one process): keeping them open would mask the
      // EOF the handler relies on when the server side closes its end.
      for (int fd = 3; fd < 1024; fd++) {
        close(fd);
      }
      execl("/bin/cat", "cat", (char*)NULL);
      _exit(127);
    }
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
    const pid_t pid = childPid.exchange(-1);
    if (pid > 0) {
      int status = 0;
      waitpid(pid, &status, 0);
    }
  }
  virtual int getFd() { return masterFd; }
  virtual void setInfo(const winsize& tmpwin) {}

  pid_t getChildPid() { return childPid.load(); }

 private:
  int masterFd;
  std::atomic<pid_t> childPid;
};

// A real-pty session whose teardown remains safe if an assertion throws.
struct RealPtySessionFixture {
  ~RealPtySessionFixture() {
    try {
      stop();
    } catch (...) {
    }
  }

  void start(RestartableServer& target) {
    auto fakeSubprocessUtils = make_shared<FakeSubprocessUtils>();
    auto sshSetupHandler =
        make_shared<FakeSshSetupHandler>(fakeSubprocessUtils);
    auto idpass = sshSetupHandler->SetupSsh("", "localhost", "localhost", 2022,
                                            "", "", false, 0, "", "", {});
    id = idpass.first;
    const string passkey = idpass.second;

    consoleSocketHandler.reset(new PipeSocketHandler());
    console.reset(new FakeConsole(consoleSocketHandler));
    realPty.reset(new RealPtyCatTerminal());
    handlerSocketHandler.reset(new PipeSocketHandler());
    handler = shared_ptr<UserTerminalHandler>(
        new UserTerminalHandler(handlerSocketHandler, realPty, true,
                                target.routerEndpoint, id + "/" + passkey));
    handlerThread = thread([this]() { handler->run(); });

    clientSocketHandler.reset(new PipeSocketHandler());
    clientPipeSocketHandler.reset(new PipeSocketHandler());
    client = shared_ptr<TerminalClient>(new TerminalClient(
        clientSocketHandler, clientPipeSocketHandler, target.serverEndpoint, id,
        passkey, console, false, "", "", false, "",
        MAX_CLIENT_KEEP_ALIVE_DURATION, vector<pair<string, string>>()));
    clientThread = thread([this]() { client->run("", false); });

    requireEventually([this]() { return console->isSetup(); }, 30,
                      "console setup");
    // The pty child is forked only after the client bootstrap delivers
    // TERMINAL_INIT to the handler.
    requireEventually([this]() { return realPty->getChildPid() > 0; }, 30,
                      "pty setup");
  }

  void stop() {
    if (client) {
      client->shutdown();
      if (clientThread.joinable()) {
        clientThread.join();
      }
      client.reset();
    }
    if (handler) {
      handler->shutdown();
      if (handlerThread.joinable()) {
        handlerThread.join();
      }
      handler.reset();
    }
  }

  string id;
  shared_ptr<PipeSocketHandler> consoleSocketHandler;
  shared_ptr<SocketHandler> handlerSocketHandler;
  shared_ptr<SocketHandler> clientSocketHandler;
  shared_ptr<SocketHandler> clientPipeSocketHandler;
  shared_ptr<FakeConsole> console;
  shared_ptr<RealPtyCatTerminal> realPty;
  shared_ptr<UserTerminalHandler> handler;
  shared_ptr<TerminalClient> client;
  thread handlerThread;
  thread clientThread;
};

void requireTerminalOutputEventually(const shared_ptr<FakeConsole>& console,
                                     const string& sentinel, int seconds,
                                     const string& what) {
  string output;
  requireEventually(
      [&]() {
        while (console->hasTerminalData()) {
          output += console->getTerminalData(1);
        }
        return output.find(sentinel) != string::npos;
      },
      seconds, what);
}

void requireKeystrokesEventually(
    const shared_ptr<FakeUserTerminal>& userTerminal, const string& sentinel,
    int seconds, const string& what) {
  string input;
  requireEventually(
      [&]() {
        while (userTerminal->hasKeystrokes()) {
          input += userTerminal->getKeystrokes(1);
        }
        return input.find(sentinel) != string::npos;
      },
      seconds, what);
}

string makePipeDir() {
  string tmpPath = GetTempDirectory() + string("et_restart_test_XXXXXXXX");
  return string(mkdtemp(&tmpPath[0]));
}

struct ScopedTestHome {
  ScopedTestHome() {
    const char* currentHome = getenv("HOME");
    if (currentHome) {
      previousHome = currentHome;
    }
    path = makePipeDir();
    ::setenv("HOME", path.c_str(), 1);
  }

  ~ScopedTestHome() {
    if (previousHome) {
      ::setenv("HOME", previousHome->c_str(), 1);
    } else {
      ::unsetenv("HOME");
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  optional<string> previousHome;
  string path;
};

}  // namespace

TEST_CASE("RouterRestartSurvival", "[RouterRestart]") {
  const string pipeDirectory = makePipeDir();
  RestartableServer target;
  target.serverEndpoint.set_name(pipeDirectory + "/pipe_server");
  target.routerEndpoint.set_name(pipeDirectory + "/pipe_router");
  target.start();

  SessionFixture session;
  session.start(target);

  // Baseline round trip before the restart.
  session.console->simulateKeystrokes("a");
  REQUIRE(session.userTerminal->getKeystrokes(1) == "a");

  // Kill the "etserver process".  The handler must keep its terminal alive.
  target.kill();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  REQUIRE_FALSE(session.userTerminal->wasCleanedUp());
  REQUIRE_FALSE(session.userTerminal->sessionEndHandled());

  // Replacement etserver on the same paths: the terminal re-registers and
  // the client resumes without any bootstrap.
  target.start();
  requireEventually(
      [&]() { return target.server->terminalRouter->isPtyActive(session.id); },
      60, "terminal re-registration");
  requireEventually(
      [&]() { return target.server->clientConnectionExists(session.id); }, 60,
      "client reconnect");

  // The session still works, in both directions.
  session.console->simulateKeystrokes("b");
  REQUIRE(session.userTerminal->getKeystrokes(1) == "b");
  session.userTerminal->simulateTerminalResponse("R");
  REQUIRE(session.console->getTerminalData(1) == "R");

  session.stop();
  target.kill();
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_server").c_str()));
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_router").c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("RouterReregistrationSurvivesWithLiveServer", "[RouterRestart]") {
  const string pipeDirectory = makePipeDir();
  RestartableServer target;
  target.serverEndpoint.set_name(pipeDirectory + "/pipe_server");
  target.routerEndpoint.set_name(pipeDirectory + "/pipe_router");
  target.start();

  SessionFixture session;
  session.start(target);
  const shared_ptr<ServerClientConnection> oldConnection =
      target.server->getClientConnection(session.id);
  const optional<TerminalUserInfo> oldInfo =
      target.server->terminalRouter->tryGetInfoForConnection(oldConnection);
  REQUIRE(oldInfo.has_value());
  const int oldTerminalFd = oldInfo->fd();

  // Break only this terminal-to-server pipe. The server process and its TCP
  // listener remain live while etterminal re-registers the same session id.
  REQUIRE(::shutdown(oldTerminalFd, SHUT_RDWR) == 0);

  requireEventually(
      [&]() {
        const optional<TerminalUserInfo> info =
            target.server->terminalRouter->tryGetInfoForConnection(
                oldConnection);
        return info && info->fd() != oldTerminalFd;
      },
      30, "replacement terminal registration");
  requireEventually(
      [&]() {
        return target.server->getClientConnection(session.id) != oldConnection;
      },
      30, "client reconnect to replacement terminal pump");

  session.console->simulateKeystrokes("N");
  requireKeystrokesEventually(session.userTerminal, "N", 30,
                              "input after terminal re-registration");
  session.userTerminal->simulateTerminalResponse("R");
  requireTerminalOutputEventually(session.console, "R", 30,
                                  "output after terminal re-registration");

  session.stop();
  target.kill();
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_server").c_str()));
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_router").c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("TerminalClientPersistsOscTitle", "[RouterRestart]") {
  ScopedTestHome home;
  const string pipeDirectory = makePipeDir();
  RestartableServer target;
  target.serverEndpoint.set_name(pipeDirectory + "/pipe_server");
  target.routerEndpoint.set_name(pipeDirectory + "/pipe_router");
  target.start();

  SessionFixture session;
  session.start(target, [](const string& title) {
    return updateSessionTitle("alpha", title);
  });
  SessionInfo info;
  info.name = "alpha";
  info.host = "localhost";
  info.port = 2022;
  info.id = session.id;
  info.passkey = session.passkey;
  info.title = "";
  info.savedAt = static_cast<int64_t>(time(NULL));
  info.lastSeenAt = 0;
  saveSession(info);

  const string output = "\033]2;Integrated Title\007";
  session.userTerminal->simulateTerminalResponse(output);
  requireEventually(
      []() {
        const optional<SessionInfo> saved = loadSession("alpha");
        return saved && saved->title == "Integrated Title";
      },
      10, "saved terminal title");
  REQUIRE(session.console->getTerminalData(output.size()) == output);

  session.stop();
  target.kill();
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_server").c_str()));
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_router").c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("RouterRestartOutageBackpressure", "[RouterRestart]") {
  const string pipeDirectory = makePipeDir();
  RestartableServer target;
  target.serverEndpoint.set_name(pipeDirectory + "/pipe_server");
  target.routerEndpoint.set_name(pipeDirectory + "/pipe_router");
  target.start();

  SessionFixture session;
  session.start(target);

  // Kill the router, then produce terminal output during the outage: the
  // handler must not consume it until a replacement router is up.
  target.kill();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  session.userTerminal->simulateTerminalResponse("OUTAGE");
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  REQUIRE_FALSE(session.userTerminal->wasCleanedUp());

  target.start();
  requireEventually(
      [&]() { return target.server->terminalRouter->isPtyActive(session.id); },
      60, "terminal re-registration");
  requireEventually(
      [&]() { return target.server->clientConnectionExists(session.id); }, 60,
      "client reconnect");

  // The bytes produced during the outage arrive intact and in order.
  REQUIRE(session.console->getTerminalData(6) == "OUTAGE");

  session.stop();
  target.kill();
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_server").c_str()));
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_router").c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("RouterRestartConcurrentReregistration", "[RouterRestart]") {
  // The 2026-08-19 incident killed 14 sessions; model exactly that count.
  const int kSessionCount = 14;
  const string pipeDirectory = makePipeDir();
  RestartableServer target;
  target.serverEndpoint.set_name(pipeDirectory + "/pipe_server");
  target.routerEndpoint.set_name(pipeDirectory + "/pipe_router");
  target.start();

  vector<shared_ptr<SessionFixture>> sessions;
  for (int i = 0; i < kSessionCount; i++) {
    auto session = make_shared<SessionFixture>();
    session->start(target);
    sessions.push_back(session);
  }

  target.kill();

  target.start();
  for (auto& session : sessions) {
    requireEventually(
        [&]() {
          return target.server->terminalRouter->isPtyActive(session->id);
        },
        90, "terminal re-registration for " + session->id);
  }
  for (auto& session : sessions) {
    requireEventually(
        [&]() { return target.server->clientConnectionExists(session->id); },
        90, "client reconnect for " + session->id);
  }

  // Every session is functional after the restart.
  for (int i = 0; i < kSessionCount; i++) {
    const char marker = char('a' + i);
    sessions[i]->console->simulateKeystrokes(string(1, marker));
  }
  for (int i = 0; i < kSessionCount; i++) {
    const char marker = char('a' + i);
    requireKeystrokesEventually(sessions[i]->userTerminal, string(1, marker),
                                60, "marker for " + sessions[i]->id);
  }

  for (auto& session : sessions) {
    session->stop();
  }
  target.kill();
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_server").c_str()));
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_router").c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

TEST_CASE("RouterRestartRealPtySurvives", "[RouterRestart]") {
  const string pipeDirectory = makePipeDir();
  RestartableServer target;
  target.serverEndpoint.set_name(pipeDirectory + "/pipe_server");
  target.routerEndpoint.set_name(pipeDirectory + "/pipe_router");
  target.start();

  RealPtySessionFixture session;
  session.start(target);
  const pid_t childBefore = session.realPty->getChildPid();

  // cat echoes input back through the whole chain.
  const string beforeRestart = "ET_REAL_PTY_BEFORE_RESTART";
  session.console->simulateKeystrokes(beforeRestart);
  requireTerminalOutputEventually(session.console, beforeRestart, 30,
                                  "real pty echo before restart");

  target.kill();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // The pty child (the stand-in for the user's shell) is untouched.
  REQUIRE(kill(childBefore, 0) == 0);

  target.start();
  requireEventually(
      [&]() { return target.server->terminalRouter->isPtyActive(session.id); },
      60, "terminal re-registration");
  requireEventually(
      [&]() { return target.server->clientConnectionExists(session.id); }, 60,
      "client reconnect");

  // Same child process, and the echo path works after the restart.
  REQUIRE(session.realPty->getChildPid() == childBefore);
  const string afterRestart = "ET_REAL_PTY_AFTER_RESTART";
  session.console->simulateKeystrokes(afterRestart);
  requireTerminalOutputEventually(session.console, afterRestart, 30,
                                  "real pty echo after restart");

  session.stop();
  target.kill();
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_server").c_str()));
  FATAL_FAIL(::remove((pipeDirectory + "/pipe_router").c_str()));
  FATAL_FAIL(::remove(pipeDirectory.c_str()));
}

}  // namespace et
#endif
