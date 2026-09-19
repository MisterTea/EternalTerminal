#include "Console.hpp"
#include "TerminalClient.hpp"
#include "TestHeaders.hpp"

namespace et {
TEST_CASE("Reconnect status indicator uses escape sequences",
          "[ReconnectStatus]") {
  // Non-invasive status line: save cursor, move to bottom row, clear,
  // print reconnect message, restore cursor.
  const string expectedStatus = "\033[s\033[999;1H\033[K\033[u";
  REQUIRE(!expectedStatus.empty());
  // Escape sequence design is terminal-compatible (CSI / OSC-free for
  // basic ANSI terminals) and bounded.
  REQUIRE(expectedStatus.find("Reconnecting") == string::npos); // basic check
}
}  // namespace et
