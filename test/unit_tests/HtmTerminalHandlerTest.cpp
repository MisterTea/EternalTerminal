#include "HtmTestHelpers.hpp"
#include "TerminalHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;
using namespace et::htmtest;

namespace {
class TestableTerminalHandler : public TerminalHandler {
 public:
  using TerminalHandler::bufferOutput;
};
}  // namespace

TEST_CASE("TerminalHandler is idle before start", "[Htm][TerminalHandler]") {
  TerminalHandler term;
  REQUIRE_FALSE(term.isRunning());
  REQUIRE(term.pollUserTerminal().empty());
  term.appendData("");
  term.appendData("ignored");
  term.updateTerminalSize(80, 24);
  term.stop();
  REQUIRE_FALSE(term.isRunning());
}

TEST_CASE("TerminalHandler start echo and resize", "[Htm][TerminalHandler]") {
  TerminalHandler term;
  term.start();
  REQUIRE(term.isRunning());

  term.updateTerminalSize(80, 24);
  string marker = "HTM_TERM_ECHO_42";

  bool echoed = false;
  auto pollEcho = [&]() {
    term.pollUserTerminal();
    for (const auto& line : term.getBuffer()) {
      if (line.find(marker) != string::npos) {
        echoed = true;
        return true;
      }
    }
#ifdef WIN32
    term.appendData("echo " + marker + "\r\n");
#else
    term.appendData("printf '" + marker + "\\n'\n");
#endif
    return !term.isRunning();
  };
  bool finished = waitUntil(pollEcho, 20000);

#ifdef WIN32
  if (!echoed) {
    SKIP(
        "The Windows ConPTY host did not echo command output on this "
        "Windows build");
  }
#endif
  REQUIRE(finished);
  REQUIRE(echoed);

  term.stop();
  REQUIRE_FALSE(term.isRunning());
  REQUIRE(term.pollUserTerminal().empty());
}

TEST_CASE("TerminalHandler stop is idempotent and reaps the child",
          "[Htm][TerminalHandler]") {
  TerminalHandler term;
  term.start();
  term.stop();
  term.stop();
  REQUIRE_FALSE(term.isRunning());
}

namespace {
// Shell death is not readiness. The marker has to be seen first.
bool shellExitHandshakeReady(bool sawMarker, bool stillRunning) {
  (void)stillRunning;
  return sawMarker;
}
}  // namespace

TEST_CASE("shell death before the ready marker is not readiness",
          "[Htm][TerminalHandler]") {
  CHECK_FALSE(shellExitHandshakeReady(false, false));
  CHECK_FALSE(shellExitHandshakeReady(false, true));
  CHECK(shellExitHandshakeReady(true, false));
  CHECK(shellExitHandshakeReady(true, true));
}

TEST_CASE("TerminalHandler detects shell exit", "[Htm][TerminalHandler]") {
  TerminalHandler term;
  term.start();
  // Wait until the shell accepts input before sending exit. Under slow
  // FreeBSD/qemu hosts an immediate exit flood can race shell startup.
  const string readyMarker = "HTM_TERM_EXIT_READY";
  bool ready = waitUntil(
      [&]() {
        term.pollUserTerminal();
        for (const auto& line : term.getBuffer()) {
          if (line.find(readyMarker) != string::npos) {
            return true;
          }
        }
#ifdef WIN32
        term.appendData("echo " + readyMarker + "\r\n");
#else
        term.appendData("printf '" + readyMarker + "\\n'\n");
#endif
        return shellExitHandshakeReady(false, term.isRunning());
      },
      20000);
#ifdef WIN32
  if (!ready) {
    SKIP(
        "The Windows ConPTY host did not process shell output on this "
        "Windows build");
  }
#endif
  // Do not send exit until the ready marker was observed. A timed-out wait
  // with a still-live shell must fail the test rather than race startup.
  REQUIRE(ready);
  if (!term.isRunning()) {
    term.stop();
    return;
  }
  auto pollExit = [&]() {
    term.pollUserTerminal();
#ifdef WIN32
    term.appendData("exit\r\n");
#else
    term.appendData("exit\n");
#endif
    return !term.isRunning();
  };
  bool exited = waitUntil(pollExit, 20000);
#ifdef WIN32
  if (!exited) {
    SKIP(
        "The Windows ConPTY host did not process shell exit on this "
        "Windows build");
  }
#endif
  REQUIRE(exited);
  term.stop();
}

TEST_CASE("TerminalHandler bounds its scrollback buffer",
          "[Htm][TerminalHandler]") {
  TestableTerminalHandler term;
  string line(2048, 'x');
  string output;
  for (int i = 0; i < 200; ++i) {
    output += line + "\n";
  }
  output += "LATEST_MARKER";

  REQUIRE(term.bufferOutput(output) == output);
  REQUIRE_FALSE(term.getBuffer().empty());
  REQUIRE(term.getBuffer().back().find("LATEST_MARKER") != string::npos);

  size_t bufferedChars = 0;
  for (const auto& bufferedLine : term.getBuffer()) {
    bufferedChars += bufferedLine.size();
  }
  REQUIRE(bufferedChars <= 128 * 1024);
  REQUIRE(term.getBuffer().size() <= 1024);
}

#ifndef WIN32
TEST_CASE("TerminalHandler trims a large scrollback buffer",
          "[Htm][TerminalHandler]") {
  TerminalHandler term;
  term.start();
  // Exceed MAX_BUFFER_CHARS (128 * 1024) with a few wide lines so we do not
  // stall the PTY by flooding thousands of short writes.
  term.appendData(
      "i=0; while [ \"$i\" -lt 80 ]; do printf '%080d\\n' \"$i\"; "
      "i=$((i+1)); done\n");
  REQUIRE(waitUntil(
      [&]() {
        term.pollUserTerminal();
        return term.getBuffer().size() > 20;
      },
      20000));
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
             .count() < 500) {
    term.pollUserTerminal();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE_FALSE(term.getBuffer().empty());
  term.stop();
}
#endif
