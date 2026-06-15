#include "ControlConsole.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
string drainFd(int fd, size_t maxBytes = 4096) {
  string buf(maxBytes, '\0');
  ssize_t rc = ::read(fd, &buf[0], maxBytes);
  if (rc <= 0) {
    return "";
  }
  buf.resize((size_t)rc);
  return buf;
}
}  // namespace

TEST_CASE("ControlConsoleInjectedInputAppearsOnFd", "[ControlConsole]") {
  ControlConsole console;
  REQUIRE(console.getFd() >= 0);

  console.injectInput("ls -la\n");
  REQUIRE(drainFd(console.getFd()) == "ls -la\n");

  // Control bytes ride the same path (Ctrl-C, Ctrl-D, ESC).
  console.injectInput(string("\x03\x04\x1b", 3));
  REQUIRE(drainFd(console.getFd()) == string("\x03\x04\x1b", 3));
}

TEST_CASE("ControlConsoleSecretInputRedactedFromTranscript",
          "[ControlConsole]") {
  ControlConsole console;

  // A secret write still reaches the shell verbatim...
  console.injectInput("hunter2\n", /*secret=*/true);
  REQUIRE(drainFd(console.getFd()) == "hunter2\n");

  // ...but the transcript (what `peep` shows) must not leak the plaintext.
  TranscriptRead tr = console.readTranscript(-1);
  REQUIRE(tr.records.size() == 1);
  REQUIRE(tr.records[0].dir == '>');
  REQUIRE(tr.records[0].bytes == "<secret>");
  REQUIRE(tr.records[0].bytes.find("hunter2") == string::npos);

  // A normal write is still recorded verbatim.
  console.injectInput("whoami\n");
  TranscriptRead tr2 = console.readTranscript(-1);
  REQUIRE(tr2.records.size() == 2);
  REQUIRE(tr2.records[1].bytes == "whoami\n");
}

TEST_CASE("ControlConsoleWriteFeedsScrollback", "[ControlConsole]") {
  ControlConsole console;
  // write() is what TerminalClient::run() calls with server output.
  console.write("hello ");
  console.write("world");

  auto r = console.readOutput(0);
  REQUIRE(r.data == "hello world");
  REQUIRE(r.truncated == false);
  REQUIRE(console.headCursor() == 11);

  // Non-destructive: a second reader from 0 sees the same bytes.
  auto r2 = console.readOutput(0);
  REQUIRE(r2.data == "hello world");

  // Cursor advances: only new bytes after the first read.
  console.write("!");
  auto r3 = console.readOutput(r.nextCursor);
  REQUIRE(r3.data == "!");
}

TEST_CASE("ControlConsoleResizeReflectedInTerminalInfo", "[ControlConsole]") {
  ControlConsole console;
  // Default size is 132x24 (the standard wide terminal).
  TerminalInfo def = console.getTerminalInfo();
  REQUIRE(def.row() == 24);
  REQUIRE(def.column() == 132);

  console.setSize(40, 120, 0, 0);
  TerminalInfo ti = console.getTerminalInfo();
  REQUIRE(ti.row() == 40);
  REQUIRE(ti.column() == 120);
}
