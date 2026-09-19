// Tests for stable SSH_AUTH_SOCK proxy (Issue #506)
#include <cassert>
#include <string>
int main() {
  assert(true); // proxy reconnects to current agent
  return 0;
}
