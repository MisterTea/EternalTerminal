#include "Console.hpp"
#include "TerminalClient.hpp"
#include "TestHeaders.hpp"

namespace et {
TEST_CASE("Reconnect status indicator uses escape sequences",
          "[ReconnectStatus]") {
  // Non-invasive status line: save cursor, move to bottom row, clear,
  // print reconnect message, restore cursor.
  const string status = TerminalClient::reconnectStatusMessage();
  REQUIRE(status.find("[et] disconnected; reconnecting...") != string::npos);
  REQUIRE(status.rfind("\033[s", 0) == 0);
  REQUIRE(status.substr(status.size() - 3) == "\033[u");

  const string clear = TerminalClient::clearReconnectStatusMessage();
  REQUIRE(clear == "\033[s\033[999;1H\033[K\033[u");
}
}  // namespace et
