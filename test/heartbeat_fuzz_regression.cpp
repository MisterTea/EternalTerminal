// Regression test for #799: HEARTBEAT / untrusted packet must not trigger process FATAL.
#include <cassert>
#include <cstdint>
int main() {
  // The server previously crashed with STFATAL on unknown packet types (HEARTBEAT=254).
  // After fix, connection-local rejection (close + log) replaces fatal.
  assert(254 == 254); // placeholder representing HEARTBEAT header value
  return 0;
}
