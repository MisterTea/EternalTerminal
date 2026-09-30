#include <fstream>

#include "MainTestHarness.hpp"
#include "TcpSocketHandler.hpp"
#include "TerminalServer.hpp"
#include "TerminalServerMain.hpp"

using namespace et;
using et::test::contains;
using et::test::MainResult;
using et::test::runMain;

namespace {
struct ServerMainFixture {
  string directory = test::makeTempDir("et_server_main");

  ~ServerMainFixture() {
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  }

  string writeConfig(const string& contents) {
    string path = directory + "/et.cfg";
    std::ofstream(path) << contents;
    return path;
  }

  // Common flags that keep etserver's logs, fifo, and telemetry inside the
  // fixture directory and its stderr on the test's stderr.
  vector<string> args(vector<string> extra) {
    vector<string> all = {
        "etserver", "--logtostdout", "--telemetry=false",  "--logdir",
        directory,  "--serverfifo",  directory + "/router"};
    all.insert(all.end(), extra.begin(), extra.end());
    return all;
  }

  MainResult run(
      const vector<string>& argv,
      const std::function<void(TerminalServer&)>& onReady =
          [](TerminalServer& server) { server.shutdown(); }) {
    return runMain(argv, [&](int argc, char** argvPtr) {
      return TerminalServerMain(argc, argvPtr, onReady);
    });
  }
};
}  // namespace

TEST_CASE_METHOD(ServerMainFixture, "etserver prints help and version",
                 "[TerminalServerMain]") {
  MainResult help = run({"etserver", "--help"});
  REQUIRE(help.exitCode == 0);
  REQUIRE(contains(help.output, "--cfgfile"));

  MainResult version = run({"etserver", "--version"});
  REQUIRE(version.exitCode == 0);
  REQUIRE(contains(version.output, "et version"));
}

TEST_CASE_METHOD(ServerMainFixture, "etserver rejects bad command line values",
                 "[TerminalServerMain]") {
  SECTION("Non-numeric port") {
    MainResult result = run(args({"--port", "abc"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Exception:"));
  }

  SECTION("Negative disconnect timeout") {
    MainResult result = run(args({"--disconnect-timeout", "-1"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "non-negative"));
  }

  SECTION("Disconnect timeout that overflows seconds") {
    MainResult result = run(args({"--disconnect-timeout", "40000000"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "too large"));
  }
}

TEST_CASE_METHOD(ServerMainFixture, "etserver rejects bad config files",
                 "[TerminalServerMain]") {
  SECTION("Missing config file") {
    MainResult result =
        run(args({"--cfgfile", directory + "/does-not-exist.cfg"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid config file"));
  }

  // Before the fix, stoi's std::invalid_argument escaped main() and aborted.
  SECTION("Non-numeric port") {
    string cfg = writeConfig("[Networking]\nport=abc\n");
    MainResult result = run(args({"--cfgfile", cfg}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "[Networking] port"));
  }

  SECTION("Port with trailing garbage") {
    string cfg = writeConfig("[Networking]\nport=2022x\n");
    MainResult result = run(args({"--cfgfile", cfg}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "[Networking] port"));
  }

  SECTION("Overflowing backlog") {
    string cfg = writeConfig("[Networking]\nbacklog=99999999999999999999999\n");
    MainResult result = run(args({"--cfgfile", cfg}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "[Networking] backlog"));
  }

  SECTION("Non-numeric disconnect timeout") {
    string cfg = writeConfig("[Networking]\ndisconnect_timeout=soon\n");
    MainResult result = run(args({"--cfgfile", cfg}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "[Networking] disconnect_timeout"));
  }

  SECTION("Negative disconnect timeout from config") {
    string cfg = writeConfig("[Networking]\ndisconnect_timeout=-5\n");
    MainResult result = run(args({"--cfgfile", cfg}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "non-negative"));
  }
}

#ifndef WIN32
// Before the fix, TcpSocketHandler::listen's runtime_error escaped main() and
// aborted etserver.
TEST_CASE_METHOD(ServerMainFixture,
                 "etserver reports a listen failure instead of aborting",
                 "[TerminalServerMain]") {
  SECTION("Unresolvable bind address") {
    MainResult result =
        run(args({"--port", std::to_string(test::unusedLoopbackPort()),
                  "--bindip", "not-a-host.invalid"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Failed to resolve address for port"));
  }

  SECTION("Port already in use") {
    test::LoopbackListener occupied;
    MainResult result = run(args(
        {"--port", std::to_string(occupied.port()), "--bindip", "127.0.0.1"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Could not bind to any interface"));
  }
}

TEST_CASE_METHOD(ServerMainFixture,
                 "etserver starts from a config file and stops cleanly",
                 "[TerminalServerMain]") {
  int port = test::unusedLoopbackPort();
  string cfg =
      writeConfig("[Networking]\nport=" + std::to_string(port) +
                  "\nbind_ip=127.0.0.1\nbacklog=8\n"
                  "disconnect_timeout=3\n"
                  "[Debug]\nverbose=1\nsilent=0\nlogsize=1048576\n"
                  "telemetry=false\nlogdirectory=" +
                  directory + "\nserverfifo=" + directory + "/cfg-router\n");

  bool reachable = false;
  MainResult result = run({"etserver", "--logtostdout", "--cfgfile", cfg},
                          [&](TerminalServer& server) {
                            SocketEndpoint endpoint;
                            endpoint.set_name("127.0.0.1");
                            endpoint.set_port(port);
                            TcpSocketHandler client;
                            int fd = client.connect(endpoint);
                            reachable = fd >= 0;
                            if (fd >= 0) {
                              client.close(fd);
                            }
                            server.shutdown();
                          });

  REQUIRE(result.exitCode == 0);
  REQUIRE(reachable);
  REQUIRE(fs::exists(directory + "/cfg-router"));
}

TEST_CASE_METHOD(ServerMainFixture,
                 "etserver command line overrides the config file",
                 "[TerminalServerMain]") {
  int port = test::unusedLoopbackPort();
  string cfg = writeConfig(
      "[Networking]\nport=1\nbind_ip=not-a-host.invalid\n"
      "disconnect_timeout=-1\n[Debug]\nverbose=3\nsilent=1\nserverfifo=\n");

  int readyPort = -1;
  MainResult result =
      run(args({"--cfgfile", cfg, "--port", std::to_string(port), "--bindip",
                "127.0.0.1", "--disconnect-timeout", "2", "-v", "1"}),
          [&](TerminalServer& server) {
            readyPort = port;
            server.shutdown();
          });

  REQUIRE(result.exitCode == 0);
  REQUIRE(readyPort == port);
  REQUIRE(fs::exists(directory + "/router"));
}
#else
TEST_CASE("etserver listen and startup paths", "[TerminalServerMain]") {
  SKIP("Router fifo paths and loopback listeners are Unix-only in this test");
}
#endif
