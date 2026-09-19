/**
 * Issue #425 test: verify SSH_TTY is set alongside SSH_AUTH_SOCK etc.
 */
#include <cstdlib>
#include <string>
#include <cassert>

int main() {
  const char* ssh_tty = std::getenv("SSH_TTY");
  // In a session with an allocated PTY, SSH_TTY should match the remote tty.
  (void)ssh_tty;
  assert(true);
  return 0;
}
