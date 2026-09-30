#include <fstream>

#include "FakeConsole.hpp"
#include "MainTestHarness.hpp"
#include "MuxMaster.hpp"
#include "SessionStore.hpp"
#include "TerminalClient.hpp"
#include "TerminalClientMain.hpp"
#include "TerminalServer.hpp"
#include "UserTerminalHandler.hpp"

#ifndef WIN32
#include <sys/un.h>
#endif

using namespace et;
using et::test::contains;
using et::test::LoopbackListener;
using et::test::MainResult;
using et::test::runMain;

namespace {
// Stands in for the bootstrap ssh: returns a canned etterminal response.
class ScriptedSubprocessUtils : public SubprocessUtils {
 public:
  explicit ScriptedSubprocessUtils(string output) : output(std::move(output)) {}

  string SubprocessToStringInteractive(const string& command,
                                       const vector<string>& args) override {
    calls++;
    lastArgs = args;
    if (!callLogPath.empty()) {
      std::ofstream log(callLogPath, std::ios::app);
      for (const auto& arg : args) {
        log << arg << ' ';
      }
      log << '\n';
    }
    return output;
  }

  string output;
  int calls = 0;
  vector<string> lastArgs;
  // Also records each call here, one line per call, so calls made in a forked
  // child stay visible to the test.
  string callLogPath;
};

const string kSessionId = "abcdefghijklmnop";
const string kSessionPasskey = "0123456789abcdef0123456789abcdef";
const string kIdPasskeyResponse =
    "IDPASSKEY:" + kSessionId + "/" + kSessionPasskey;

class ScopedEnv {
 public:
  ScopedEnv(const string& name, const char* value) : name(name) {
    const char* old = getenv(name.c_str());
    if (old) {
      saved = string(old);
    }
    set(value);
  }
  ~ScopedEnv() { set(saved ? saved->c_str() : nullptr); }

 private:
  void set(const char* value) {
#ifdef WIN32
    _putenv_s(name.c_str(), value ? value : "");
#else
    if (value) {
      ::setenv(name.c_str(), value, 1);
    } else {
      ::unsetenv(name.c_str());
    }
#endif
  }

  string name;
  optional<string> saved;
};

struct ClientMainFixture {
  string directory = test::makeTempDir("et_client_main");
  ScopedEnv home{"HOME", directory.c_str()};
  shared_ptr<ScriptedSubprocessUtils> ssh =
      make_shared<ScriptedSubprocessUtils>(kIdPasskeyResponse);

  ~ClientMainFixture() {
    TerminalClient::configureCloseOnHangup(false);
    TerminalClient::resetHangupClose();
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  }

  // Common flags keep logs in the fixture directory, stderr on the test's
  // stderr, and telemetry off. Host and command operands go last in `extra`.
  vector<string> args(vector<string> extra) {
    vector<string> all = {"et", "--logdir", directory, "--logtostdout",
                          "--telemetry=false"};
    all.insert(all.end(), extra.begin(), extra.end());
    return all;
  }

  MainResult run(const vector<string>& argv) {
    TerminalClientMainHooks hooks;
    hooks.subprocessUtils = ssh;
    hooks.onClientReady = onClientReady;
    // et creates its own TelemetryService; retire the runner's first so it
    // is not destroyed without a shutdown. runMain restores one afterwards.
    if (TelemetryService::exists()) {
      TelemetryService::get()->shutdown();
      TelemetryService::destroy();
    }
    return runMain(argv, [&](int argc, char** argvPtr) {
      return TerminalClientMain(argc, argvPtr, hooks);
    });
  }

#ifndef WIN32
  test::ChildMainResult runInChild(const vector<string>& argv) {
    ssh->callLogPath = directory + "/ssh-calls";
    TerminalClientMainHooks hooks;
    hooks.subprocessUtils = ssh;
    return test::runMainInChild(argv, [&](int argc, char** argvPtr) {
      if (TelemetryService::exists()) {
        TelemetryService::get()->shutdown();
        TelemetryService::destroy();
      }
      return TerminalClientMain(argc, argvPtr, hooks);
    });
  }

  vector<string> recordedSshCalls() {
    vector<string> calls;
    std::ifstream log(directory + "/ssh-calls");
    for (string line; std::getline(log, line);) {
      calls.push_back(line);
    }
    return calls;
  }
#endif

  SessionInfo saveTestSession(const string& name, int port,
                              const string& title = "",
                              const string& id = kSessionId) {
    SessionInfo session;
    session.name = name;
    session.host = "127.0.0.1";
    session.port = port;
    session.id = id;
    session.passkey = kSessionPasskey;
    session.title = title;
    session.savedAt = static_cast<int64_t>(time(NULL));
    session.lastSeenAt = session.savedAt;
    saveSession(session);
    return session;
  }

  std::function<void(TerminalClient&)> onClientReady;
};

#ifndef WIN32
// A real etserver on a loopback port with one registered terminal whose
// id/passkey match kIdPasskeyResponse, so the scripted ssh bootstrap yields a
// session the client can actually connect to.
//
// The server runs in a child process; see test::BackgroundChild.
struct LiveServer {
  explicit LiveServer(const string& directory)
      : port(test::unusedLoopbackPort()),
        process([this, directory]() { serve(directory); }) {
    REQUIRE(process.started);
  }

  string portArg() const { return std::to_string(port); }

  const int port;

 private:
  // Runs in the child, which is killed rather than shut down, so everything
  // here is intentionally leaked.
  void serve(const string& directory) const {
    auto routerSocketHandler = make_shared<PipeSocketHandler>();
    SocketEndpoint serverEndpoint;
    serverEndpoint.set_name("127.0.0.1");
    serverEndpoint.set_port(port);
    SocketEndpoint routerEndpoint;
    routerEndpoint.set_name(directory + "/router");
    auto server = make_shared<TerminalServer>(
        make_shared<TcpSocketHandler>(), serverEndpoint, routerSocketHandler,
        routerEndpoint);
    thread([server]() { server->run(); }).detach();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto handler = make_shared<UserTerminalHandler>(
        routerSocketHandler, make_shared<FakeUserTerminal>(routerSocketHandler),
        true, routerEndpoint, kSessionId + "/" + kSessionPasskey);
    thread([handler]() {
      try {
        handler->run();
      } catch (const std::exception&) {
        // A killed session tears the terminal down underneath run().
      }
    }).detach();
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  test::BackgroundChild process;
};
#endif
}  // namespace

TEST_CASE_METHOD(ClientMainFixture, "et prints help and version",
                 "[TerminalClientMain]") {
  MainResult help = run({"et", "--help"});
  REQUIRE(help.exitCode == 0);
  REQUIRE(contains(help.output, "OpenSSH mux"));

  MainResult version = run({"et", "--version"});
  REQUIRE(version.exitCode == 0);
  REQUIRE(contains(version.output, "et version"));

  MainResult sshVersion = run({"et", "-V"});
  REQUIRE(sshVersion.exitCode == 0);
  REQUIRE_FALSE(sshVersion.output.empty());

  MainResult noHost = run(args({}));
  REQUIRE(noHost.exitCode == 0);
  REQUIRE(contains(noHost.output, "Missing host to connect to"));
}

TEST_CASE_METHOD(ClientMainFixture, "et rejects bad option values",
                 "[TerminalClientMain]") {
  SECTION("Non-numeric port") {
    MainResult result = run(args({"--port", "abc", "somehost"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Exception:"));
  }

  SECTION("Option given twice") {
    MainResult result =
        run(args({"--keepalive", "5", "--keepalive", "6", "somehost"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "must be specified only once"));
  }

  SECTION("Keepalive out of range") {
    MainResult result = run(args({"--keepalive", "0", "somehost"}));
    REQUIRE(result.exitCode == 0);
    REQUIRE(contains(result.output, "Keep-alive duration must"));
  }

  SECTION("Negative disconnect timeout") {
    MainResult result = run(args({"--disconnect-timeout", "-1", "somehost"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "non-negative"));
  }

  SECTION("Disconnect timeout that overflows seconds") {
    MainResult result =
        run(args({"--disconnect-timeout", "40000000", "somehost"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "too large"));
  }

  SECTION("Malformed mux option") {
    MainResult result = run(args({"-o", "ControlMaster=sometimes", "host"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Exception:"));
  }

  SECTION("Non-numeric sshd port") {
    MainResult result = run(args({"-p", "ssh", "somehost"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid sshd port"));
  }

  SECTION("Out-of-range sshd port") {
    MainResult result = run(args({"--no-ssh-config", "-p", "70000", "host"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid sshd port"));
  }

  SECTION("Invalid session option") {
    MainResult result =
        run(args({"--no-ssh-config", "-o", "KeyWithoutValue", "host"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid -o option"));
  }
}

TEST_CASE_METHOD(ClientMainFixture, "et rejects bad destinations",
                 "[TerminalClientMain]") {
  SECTION("Too many colons") {
    MainResult result = run(args({"a:b:c"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid host positional arg"));
  }

  SECTION("Non-numeric port suffix") {
    MainResult result = run(args({"somehost:ssh"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid host positional arg"));
  }

  // std::out_of_range used to escape the std::invalid_argument handler.
  SECTION("Overflowing port suffix") {
    MainResult result = run(args({"somehost:99999999999999999999"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid host positional arg"));
  }
}

TEST_CASE_METHOD(ClientMainFixture, "et rejects conflicting flags",
                 "[TerminalClientMain]") {
  struct Case {
    vector<string> extra;
    string message;
  };
  vector<Case> cases = {
      {{"--kill", "a", "--name", "b"}, "--kill takes a saved session name"},
      {{"--no-persist", "--name", "a", "host"}, "--no-persist cannot be"},
      {{"--attach", "a", "--name", "b"}, "--attach takes a session name"},
      {{"--attach", "a", "--tunnel", "8080:80"},
       "--attach cannot be combined with port forwarding"},
      {{"-T", "--name", "a", "host"}, "-T/--no-pty sessions are not saved"},
      {{"-N", "--command", "ls", "host"}, "-N cannot be combined"},
      {{"--ssh-config", "/etc/ssh/ssh_config", "--no-ssh-config", "host"},
       "mutually exclusive"},
      {{"--ssh-config", "relative/config", "host"}, "must be an absolute path"},
      {{"--no-ssh-config", "-T", "host"}, "-T/--no-pty requires --command"},
      {{"-O", "check", "host"}, "-O requires ControlPath"},
  };
#ifndef WIN32
  cases.push_back({{"--ssh-config", "/tmp/has space", "host"},
                   "must contain only ASCII letters"});
  cases.push_back({{"--ssh-config", "/nonexistent/et-config", "host"},
                   "must name a readable"});
#endif
  for (const auto& testCase : cases) {
    INFO("args: " << testCase.extra.size() << " first=" << testCase.extra[0]);
    MainResult result = run(args(testCase.extra));
    INFO(result.output);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, testCase.message));
  }
}

#ifndef WIN32
TEST_CASE_METHOD(ClientMainFixture, "et prints resolved config with -G",
                 "[TerminalClientMain]") {
  SECTION("Without ssh config") {
    MainResult result =
        run(args({"--no-ssh-config", "-G", "-o", "HostName=real.example", "-o",
                  "User=bob", "-p", "2200", "alias"}));
    REQUIRE(result.exitCode == 0);
    REQUIRE(contains(result.output, "host alias"));
    REQUIRE(contains(result.output, "hostname real.example"));
    REQUIRE(contains(result.output, "user bob"));
    REQUIRE(contains(result.output, "port 2200"));
  }

  SECTION("With an explicit ssh config") {
    string config = directory + "/ssh_config";
    std::ofstream(config) << "Host alias\n  HostName from-config.example\n"
                             "  User carol\n  ServerAliveInterval 600\n";
    MainResult result =
        run(args({"--ssh-config", config, "-G", "-l", "dave", "alias"}));
    REQUIRE(result.exitCode == 0);
    REQUIRE(contains(result.output, "hostname from-config.example"));
    REQUIRE(contains(result.output, "user dave"));
  }

  SECTION("With the ambient ssh config") {
    MainResult result = run(args({"-G", "-o", "User=frank", "alias"}));
    REQUIRE(result.exitCode == 0);
    REQUIRE(contains(result.output, "host alias"));
    REQUIRE(contains(result.output, "user frank"));
  }
}

TEST_CASE_METHOD(ClientMainFixture, "et talks to a missing ControlMaster",
                 "[TerminalClientMain]") {
  const string missing = directory + "/no-master.sock";

  SECTION("-O check") {
    MainResult result = run(args({"-O", "check", "-S", missing, "host"}));
    REQUIRE(result.exitCode != 0);
  }

  SECTION("-O forward") {
    MainResult result =
        run(args({"-O", "forward", "-S", missing, "-L", "8080:x:80", "host"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Control socket connect failed"));
  }

  SECTION("Passenger attach to a dead socket") {
    // A bound, non-listening socket looks like a master to
    // shouldAttachToMuxMaster, but connect() is refused.
    const string stale = directory + "/stale.sock";
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, stale.c_str(), sizeof(address.sun_path) - 1);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0);
    MainResult result = run(args({"-S", stale, "host"}));
    ::close(fd);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Failed to attach to ControlPath"));
  }
}

TEST_CASE_METHOD(ClientMainFixture, "et manages saved sessions",
                 "[TerminalClientMain]") {
  const int deadPort = test::unusedLoopbackPort();

  SECTION("Empty list") {
    MainResult result = run(args({"--list"}));
    REQUIRE(result.exitCode == 0);
    REQUIRE(contains(result.output, "LAST SEEN"));
  }

  SECTION("List shows saved sessions and truncates long titles") {
    saveTestSession("short", deadPort, "vim");
    saveTestSession("long", deadPort,
                    "a very long window title that will not fit the column");
    MainResult result = run(args({"--list"}));
    REQUIRE(result.exitCode == 0);
    REQUIRE(contains(result.output, "short"));
    REQUIRE(contains(result.output, "vim"));
    REQUIRE(contains(result.output, "…"));
  }

  SECTION("Kill or attach an unknown session") {
    saveTestSession("present", deadPort);
    MainResult kill = run(args({"--kill", "absent"}));
    REQUIRE(kill.exitCode == 1);
    REQUIRE(contains(kill.output, "No saved session named 'absent'"));
    REQUIRE(contains(kill.output, "present"));

    MainResult attach = run(args({"--attach", "absent"}));
    REQUIRE(attach.exitCode == 1);
  }

  SECTION("Ambiguous query") {
    saveTestSession("demo-one", deadPort);
    saveTestSession("demo-two", deadPort);
    MainResult result = run(args({"--attach", "demo"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Multiple saved sessions match"));
  }

  SECTION("Unreachable server keeps the record") {
    saveTestSession("gone", deadPort, "shell");
    MainResult kill = run(args({"--kill", "gone"}));
    REQUIRE(kill.exitCode == 1);
    REQUIRE(contains(kill.output, "Could not reach the ET server"));
    REQUIRE(contains(kill.output, "was not removed"));

    MainResult attach = run(args({"--attach", "shell", "--no-terminal"}));
    REQUIRE(attach.exitCode == 1);
    REQUIRE(contains(attach.output, "Could not reach the ET server"));
    REQUIRE(loadSession("gone"));
  }

  SECTION("Kill against a server that never answers") {
    LoopbackListener listener;
    saveTestSession("silent", listener.port());
    MainResult result = run(args({"--kill", "silent"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Could not kill session 'silent'"));
  }

  SECTION("--name for a session saved elsewhere") {
    saveTestSession("pinned", deadPort);
    MainResult result = run(args({"--name", "pinned", "otherhost"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "is saved for"));
  }

  SECTION("--name reattaches to a matching saved session") {
    saveTestSession("pinned", deadPort);
    MainResult result = run(args({"--name", "pinned", "--no-terminal", "--port",
                                  std::to_string(deadPort), "127.0.0.1"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Could not reach the ET server"));
  }

  SECTION("Invalid session name") {
    MainResult result = run(args({"--name", "bad/name", "host"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid session name"));
  }

  SECTION("Jumphost cannot reattach a named session") {
    saveTestSession("pinned", deadPort);
    MainResult result = run(args({"--no-ssh-config", "--name", "pinned", "-J",
                                  "jump.example", "127.0.0.1"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "cannot be reattached through a jumphost"));
  }
}

TEST_CASE_METHOD(ClientMainFixture,
                 "et reports an unreachable server without running ssh",
                 "[TerminalClientMain]") {
  const string port = std::to_string(test::unusedLoopbackPort());
  MainResult result =
      run(args({"--no-ssh-config", "--port", port, "127.0.0.1"}));
  REQUIRE(result.exitCode == 1);
  REQUIRE(contains(result.output, "Could not reach the ET server"));
  REQUIRE(ssh->calls == 0);
  REQUIRE(listSessions().empty());
}

TEST_CASE_METHOD(ClientMainFixture, "et reports a failed ssh bootstrap",
                 "[TerminalClientMain]") {
  LoopbackListener listener;
  ssh->output = "Permission denied (publickey).";
  MainResult result =
      run(args({"--no-ssh-config", "--no-persist", "--no-terminal", "--port",
                std::to_string(listener.port()), "127.0.0.1"}));
  REQUIRE(result.exitCode == 1);
  REQUIRE(ssh->calls == 1);
}

// These setup failures happen inside TerminalClient after ssh succeeds, where
// it calls exit(1), throws from stoi, or retries a missing writer forever.
// Each run happens in a child process so that cannot take et-test down.
//
// TODO(#873): TerminalClient, TunnelUtils, and Connection still have these
// bugs on this branch, so the test case is tagged [!shouldfail] and passes
// while they remain. Once #873 lands the test starts passing, which
// [!shouldfail] reports as a failure: drop the tag then.
TEST_CASE_METHOD(ClientMainFixture,
                 "et exits cleanly when session setup fails after ssh",
                 "[TerminalClientMain][!shouldfail]") {
  LoopbackListener listener;
  const string port = std::to_string(listener.port());

  SECTION("Initial connection never completes") {
    test::ChildMainResult result =
        runInChild(args({"--no-ssh-config", "--no-persist", "--no-terminal",
                         "--port", port, "127.0.0.1"}));
    INFO(result.output);
    REQUIRE(result.returned);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Could not make initial connection"));
    REQUIRE(recordedSshCalls().size() == 1);
  }

  SECTION("Non-numeric port in an ssh-style tunnel") {
    test::ChildMainResult result = runInChild(
        args({"--no-ssh-config", "--no-persist", "--no-terminal", "--port",
              port, "-L", "localhost:notaport:remote:22", "127.0.0.1"}));
    INFO(result.output);
    REQUIRE(result.returned);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid tunnel argument"));
  }

  SECTION("Agent forwarding without SSH_AUTH_SOCK") {
    ScopedEnv noAgent("SSH_AUTH_SOCK", nullptr);
    test::ChildMainResult result =
        runInChild(args({"--no-ssh-config", "--no-persist", "--no-terminal",
                         "--forward-ssh-agent", "--port", port, "127.0.0.1"}));
    INFO(result.output);
    REQUIRE(result.returned);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Missing environment variable"));
  }

  SECTION("Saved session with forwarding, default name, and a pipe console") {
    test::ChildMainResult result = runInChild(args({"--no-ssh-config",
                                                    "--port",
                                                    port,
                                                    "-T",
                                                    "--tunnel",
                                                    "18080:80",
                                                    "-R",
                                                    "19090:localhost:90",
                                                    "-D",
                                                    "11080",
                                                    "--ssh-option",
                                                    "BatchMode=yes",
                                                    "--terminal-path",
                                                    "/opt/et/etterminal",
                                                    "--ssh-socket",
                                                    "/tmp/agent.sock",
                                                    "--close-on-hangup",
                                                    "--verbose",
                                                    "1",
                                                    "--silent",
                                                    "--disconnect-timeout",
                                                    "5",
                                                    "127.0.0.1",
                                                    "echo",
                                                    "hi"}));
    INFO(result.output);
    REQUIRE(result.returned);
    REQUIRE(result.exitCode == 1);
    REQUIRE(recordedSshCalls().size() == 1);
  }

  SECTION("Named session with a pty console") {
    test::ChildMainResult result =
        runInChild(args({"--no-ssh-config", "--name", "fresh", "--port", port,
                         "--macserver", "-k", "3", "127.0.0.1"}));
    INFO(result.output);
    REQUIRE(result.returned);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Could not make initial connection"));
    // The record stays so the session can be retried with --attach.
    REQUIRE(loadSession("fresh"));
  }

  SECTION("Jumphost") {
    test::ChildMainResult result =
        runInChild(args({"--no-ssh-config", "--no-terminal", "--jport", port,
                         "-J", "jumper@127.0.0.1:22", "dest.example"}));
    INFO(result.output);
    REQUIRE(result.returned);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Sessions using a jumphost are not saved"));
    REQUIRE_FALSE(recordedSshCalls().empty());
    REQUIRE(listSessions().empty());
  }

  SECTION("Jumphost and forwarding from an ssh config") {
    string config = directory + "/ssh_config";
    std::ofstream(config) << "Host dest\n  ProxyJump jumpalias\n"
                             "  IdentityAgent /tmp/et-agent.sock\n"
                             "  ForwardAgent yes\n"
                             "  LocalForward 18081 localhost:81\n"
                             "Host jumpalias\n  HostName 127.0.0.1\n"
                             "  User jumpuser\n";
    test::ChildMainResult result = runInChild(
        args({"--ssh-config", config, "--no-terminal", "--jport", port, "-u",
              "alice", "--serverfifo", directory + "/sfifo", "--jserverfifo",
              directory + "/jfifo", "dest"}));
    INFO(result.output);
    REQUIRE(result.returned);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Sessions using a jumphost are not saved"));
    const vector<string> sshCalls = recordedSshCalls();
    REQUIRE_FALSE(sshCalls.empty());
    INFO(sshCalls.back());
    REQUIRE(contains(sshCalls.back(), "jumpuser@127.0.0.1"));
  }

  SECTION("Control session") {
    const string socketPath = directory + "/ctl/demo.sock";
    test::ChildMainResult result = runInChild(
        args({"--no-ssh-config", "--no-persist", "--ctl", "--ctl-socket",
              socketPath, "--port", port, "127.0.0.1"}));
    INFO(result.output);
    REQUIRE(result.returned);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "control socket: " + socketPath));
    REQUIRE(contains(result.output, "Could not make initial connection"));
  }

  SECTION("Control session in the default directory") {
    test::ChildMainResult result =
        runInChild(args({"--no-ssh-config", "--name", "ctl-demo", "--ctl",
                         "--port", port, "127.0.0.1"}));
    INFO(result.output);
    REQUIRE(result.returned);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "et control session: ctl-demo"));
  }
}

TEST_CASE_METHOD(ClientMainFixture,
                 "et reports a control socket it cannot create",
                 "[TerminalClientMain]") {
  const string blocker = directory + "/not-a-dir";
  std::ofstream(blocker) << "file";
  MainResult result =
      run(args({"--no-ssh-config", "--no-persist", "--ctl", "--ctl-socket",
                blocker + "/sub/demo.sock", "127.0.0.1"}));
  REQUIRE(result.exitCode == 1);
  REQUIRE(contains(result.output, "Could not prepare control socket"));
}

TEST_CASE_METHOD(ClientMainFixture, "et connects, reattaches, and kills",
                 "[TerminalClientMain]") {
  LiveServer live(directory);
  int readyCount = 0;
  onClientReady = [&readyCount](TerminalClient& client) {
    readyCount++;
    client.shutdown();
  };

  MainResult fresh =
      run(args({"--no-ssh-config", "--name", "live", "--no-terminal", "--port",
                live.portArg(), "127.0.0.1"}));
  INFO(fresh.output);
  REQUIRE(fresh.exitCode == 0);
  REQUIRE(readyCount == 1);
  REQUIRE(ssh->calls == 1);
  REQUIRE(loadSession("live"));

  MainResult attach = run(args({"--attach", "live", "--no-terminal"}));
  INFO(attach.output);
  REQUIRE(attach.exitCode == 0);
  REQUIRE(readyCount == 2);

  MainResult rename =
      run(args({"--no-ssh-config", "--name", "live", "--no-terminal", "--port",
                live.portArg(), "127.0.0.1"}));
  INFO(rename.output);
  REQUIRE(rename.exitCode == 0);
  REQUIRE(readyCount == 3);
  // Reattaching by --name reuses the saved credentials instead of ssh.
  REQUIRE(ssh->calls == 1);

  // Copies of the record outlive the session once it is killed. The server
  // only reports a removed session as gone; an id it never had is retried
  // during its restart grace window.
  saveTestSession("stale-attach", live.port);
  saveTestSession("stale-kill", live.port);

  MainResult kill = run(args({"--kill", "live"}));
  INFO(kill.output);
  REQUIRE(kill.exitCode == 0);
  REQUIRE(contains(kill.output, "Killed session 'live'"));
  REQUIRE_FALSE(loadSession("live"));

  MainResult staleAttach =
      run(args({"--attach", "stale-attach", "--no-terminal"}));
  INFO(staleAttach.output);
  REQUIRE(staleAttach.exitCode == 1);
  REQUIRE(contains(staleAttach.output, "is no longer running"));
  REQUIRE_FALSE(loadSession("stale-attach"));

  MainResult staleKill = run(args({"--kill", "stale-kill"}));
  INFO(staleKill.output);
  REQUIRE(staleKill.exitCode == 0);
  REQUIRE(contains(staleKill.output, "already gone"));
  REQUIRE_FALSE(loadSession("stale-kill"));
}

// The fresh session reuses the killed session's credentials (the scripted ssh
// always returns them), so its initial connection fails inside TerminalClient.
// TODO(#873): TerminalClient still calls exit(1) there on this branch; drop
// [!shouldfail] once #873 lands and this starts passing.
TEST_CASE_METHOD(ClientMainFixture,
                 "et replaces a named session the server no longer has",
                 "[TerminalClientMain][!shouldfail]") {
  LiveServer live(directory);
  onClientReady = [](TerminalClient& client) { client.shutdown(); };

  MainResult fresh =
      run(args({"--no-ssh-config", "--name", "live", "--no-terminal", "--port",
                live.portArg(), "127.0.0.1"}));
  REQUIRE(fresh.exitCode == 0);
  saveTestSession("stale-name", live.port);
  MainResult kill = run(args({"--kill", "live"}));
  REQUIRE(kill.exitCode == 0);

  test::ChildMainResult staleName = runInChild(
      args({"--no-ssh-config", "--name", "stale-name", "--no-terminal",
            "--port", live.portArg(), "127.0.0.1"}));
  INFO(staleName.output);
  REQUIRE(contains(staleName.output, "creating a fresh session"));
  REQUIRE(recordedSshCalls().size() == 1);
  REQUIRE(staleName.returned);
  REQUIRE(staleName.exitCode == 1);
}

TEST_CASE_METHOD(ClientMainFixture, "et runs a live session as ControlMaster",
                 "[TerminalClientMain]") {
  LiveServer live(directory);
  const string controlPath = directory + "/master.sock";
  bool masterListening = false;
  onClientReady = [&](TerminalClient& client) {
    masterListening = fs::exists(controlPath);
    client.shutdown();
  };

  MainResult result = run(
      args({"--no-ssh-config", "--no-persist", "--no-terminal", "-o",
            "ControlMaster=yes", "-o", "ControlPath=" + controlPath, "--port",
            live.portArg(), "-o", "ServerAliveInterval=1", "127.0.0.1"}));
  INFO(result.output);
  REQUIRE(result.exitCode == 0);
  REQUIRE(masterListening);
  REQUIRE_FALSE(fs::exists(controlPath));
}

TEST_CASE_METHOD(ClientMainFixture, "et talks to a running ControlMaster",
                 "[TerminalClientMain]") {
  const string controlPath = directory + "/master.sock";
  // The master runs in a child process; see test::BackgroundChild.
  auto startMaster = [&controlPath](bool withPassengerHandler) {
    return make_unique<test::BackgroundChild>([controlPath,
                                               withPassengerHandler]() {
      auto* master = new MuxMaster(controlPath, ControlPersistConfig());
      if (withPassengerHandler) {
        master->setPassengerSessionHandler(
            [](int, int, int, const string&, bool) -> uint32_t { return 7; });
      }
      master->start();
    });
  };
  unique_ptr<test::BackgroundChild> master;

  SECTION("-O check") {
    master = startMaster(false);
    MainResult result = run(args({"-O", "check", "-S", controlPath, "host"}));
    INFO(result.output);
    REQUIRE(result.exitCode == 0);
  }

  SECTION("-O forward without a forward") {
    master = startMaster(false);
    MainResult result = run(args({"-O", "forward", "-S", controlPath, "host"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "requires -L or --tunnel"));
  }

  SECTION("-O forward then cancel") {
    master = startMaster(false);
    const string spec =
        std::to_string(test::unusedLoopbackPort()) + ":localhost:80";
    MainResult forward =
        run(args({"-O", "forward", "-S", controlPath, "-L", spec, "host"}));
    INFO(forward.output);
    REQUIRE(forward.exitCode == 0);

    MainResult cancel =
        run(args({"-O", "cancel", "-S", controlPath, "-L", spec, "host"}));
    INFO(cancel.output);
    REQUIRE(cancel.exitCode == 0);
  }

  SECTION("Passenger without a session handler") {
    master = startMaster(false);
    const string spec =
        std::to_string(test::unusedLoopbackPort()) + ":localhost:80";
    MainResult result =
        run(args({"-S", controlPath, "-L", spec, "host", "true"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Mux new session failed"));
  }

  SECTION("Passenger -N with a command") {
    master = startMaster(false);
    MainResult result = run(args({"-S", controlPath, "-N", "host", "ls"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "-N cannot be combined"));
  }

  SECTION("Passenger returns the remote exit status") {
    master = startMaster(true);
    MainResult result = run(args({"-S", controlPath, "-T", "host", "true"}));
    INFO(result.output);
    REQUIRE(result.exitCode == 7);
  }
}
#else
TEST_CASE("et session, mux, and connection paths", "[TerminalClientMain]") {
  SKIP("Saved sessions, mux sockets, and --ctl are unavailable on Windows");
}
#endif
