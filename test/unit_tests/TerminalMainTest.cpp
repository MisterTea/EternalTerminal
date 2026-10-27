#include <fstream>

#include "MainTestHarness.hpp"
#include "TerminalMain.hpp"

using namespace et;
using et::test::contains;
using et::test::MainResult;
using et::test::runMain;

namespace {
const string kIdPasskey = "abcdefghijklmnop/0123456789abcdef0123456789abcdef";

struct TerminalMainFixture {
  string directory = test::makeTempDir("et_terminal_main");
  // Nothing listens here, so connecting to the router must fail.
  string missingRouter = directory + "/no-router";

  ~TerminalMainFixture() {
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  }

  vector<string> args(vector<string> extra) {
    vector<string> all = {"etterminal", "--logdir", directory, "--serverfifo",
                          missingRouter};
    all.insert(all.end(), extra.begin(), extra.end());
    return all;
  }

  MainResult run(const vector<string>& argv) {
    return runMain(argv, TerminalMain);
  }

#ifndef WIN32
  // Runs etterminal with fd 0 replaced by a pipe. Writes `input` and closes
  // the pipe unless `keepOpen`, in which case nothing ever arrives.
  MainResult runWithStdin(const vector<string>& argv, const string& input,
                          bool keepOpen = false) {
    int pipeFds[2];
    FATAL_FAIL(::pipe(pipeFds));
    if (!input.empty()) {
      RawSocketUtils::writeAll(pipeFds[1], input.data(), input.size());
    }
    if (!keepOpen) {
      ::close(pipeFds[1]);
      pipeFds[1] = -1;
    }
    int savedStdin = ::dup(STDIN_FILENO);
    FATAL_FAIL(savedStdin);
    FATAL_FAIL(::dup2(pipeFds[0], STDIN_FILENO));
    ::close(pipeFds[0]);

    MainResult result = run(argv);

    FATAL_FAIL(::dup2(savedStdin, STDIN_FILENO));
    ::close(savedStdin);
    if (pipeFds[1] >= 0) {
      ::close(pipeFds[1]);
    }
    std::cin.clear();
    clearerr(stdin);
    return result;
  }
#endif
};
}  // namespace

TEST_CASE_METHOD(TerminalMainFixture, "etterminal prints help",
                 "[TerminalMain]") {
  MainResult result = run({"etterminal", "--help"});
  REQUIRE(result.exitCode == 0);
  REQUIRE(contains(result.output, "--idpasskey"));
}

TEST_CASE_METHOD(TerminalMainFixture, "etterminal rejects bad arguments",
                 "[TerminalMain]") {
  SECTION("Non-numeric option value") {
    MainResult result =
        run(args({"--idpasskey", kIdPasskey, "--dstport", "abc"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Exception:"));
  }

  SECTION("idpasskey without a separator") {
    MainResult result = run(args({"--idpasskey", "no-separator"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid idpasskey"));
  }

  SECTION("idpasskey with an empty passkey") {
    MainResult result = run(args({"--idpasskey", "abcdefghijklmnop/"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid idpasskey"));
  }
}

#ifndef WIN32
// Before the fix, the router connection failure was a runtime_error that
// escaped main() (or an STFATAL in UserTerminalHandler), aborting etterminal.
TEST_CASE_METHOD(TerminalMainFixture,
                 "etterminal exits cleanly when etserver is not running",
                 "[TerminalMain]") {
  SECTION("Terminal mode") {
    MainResult result = run(args({"--idpasskey", kIdPasskey}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Error connecting to router"));
    REQUIRE_FALSE(contains(result.output, "IDPASSKEY:"));
  }

  SECTION("Terminal mode with an idpasskey file") {
    string keyFile = directory + "/idpasskey";
    std::ofstream(keyFile) << kIdPasskey << "\n";
    MainResult result = run(args({"--idpasskeyfile", keyFile, "-v", "1"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Error connecting to router"));
  }

  SECTION("Jumphost mode") {
    MainResult result =
        run(args({"--jump", "--dsthost", "127.0.0.1", "--dstport", "2022",
                  "--idpasskey", kIdPasskey, "--logtostdout"}));
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "communicating with et daemon"));
    // The caller must not be told the jumphost started.
    REQUIRE_FALSE(contains(result.output, "IDPASSKEY:"));
  }
}

TEST_CASE_METHOD(TerminalMainFixture, "etterminal reads idpasskey from stdin",
                 "[TerminalMain]") {
  SECTION("No input arrives") {
    MainResult result = runWithStdin(args({}), "", /*keepOpen=*/true);
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Call etterminal with --idpasskey"));
  }

  SECTION("Stdin closes without a line") {
    MainResult result = runWithStdin(args({}), "");
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Call etterminal with --idpasskey"));
  }

  SECTION("Malformed line") {
    MainResult result = runWithStdin(args({}), "garbage\n");
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Invalid stdin line"));
  }

  SECTION("Valid line reaches the router") {
    const char* savedTerm = getenv("TERM");
    string restoreTerm = savedTerm ? savedTerm : "";
    MainResult result =
        runWithStdin(args({}), kIdPasskey + "_xterm-256color\n");
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Error connecting to router"));
    REQUIRE(string(getenv("TERM")) == "xterm-256color");
    if (savedTerm) {
      setenv("TERM", restoreTerm.c_str(), 1);
    } else {
      unsetenv("TERM");
    }
  }

  SECTION("New-client placeholder is replaced") {
    MainResult result = runWithStdin(
        args({"--idpasskey", ""}),
        "XXXXXXXXXXXXXXXX/XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX_xterm\n");
    REQUIRE(result.exitCode == 1);
    REQUIRE(contains(result.output, "Error connecting to router"));
  }
}
#else
TEST_CASE("etterminal router and stdin paths", "[TerminalMain]") {
  SKIP("Router fifo paths and stdin pipes are Unix-only in this test");
}
#endif
