#include "TestHeaders.hpp"

#ifndef WIN32
#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#elif __NetBSD__
#else
#include <pty.h>
#endif

#include "HtmTestHelpers.hpp"
#endif

TEST_CASE("SSH_TTY environment variable", "[terminal]") {
#ifdef WIN32
  SKIP("SSH_TTY is only set for POSIX PTY sessions");
#else
  // When a remote PTY is allocated, SSH_TTY must point to the tty device.
  // In PseudoUserTerminal::runTerminal(), ttyname(STDIN_FILENO) is used
  // to set SSH_TTY before launching the user shell.
  int masterFd = -1;
  int slaveFd = -1;
  char name[1024] = {0};
  int rc = openpty(&masterFd, &slaveFd, name, NULL, NULL);
  REQUIRE(rc == 0);
  const char* tty = ttyname(slaveFd);
  REQUIRE(tty != nullptr);
  CHECK(std::string(tty) == std::string(name));
  close(masterFd);
  close(slaveFd);

  if (et::htmtest::runningUnderThreadSanitizer()) {
    return;
  }

  masterFd = -1;
  pid_t pid = forkpty(&masterFd, nullptr, nullptr, nullptr);
  REQUIRE(pid >= 0);

  if (pid == 0) {
    const char* slaveTty = ttyname(STDIN_FILENO);
    if (slaveTty) {
      setenv("SSH_TTY", slaveTty, 1);
    }
    const char* sshTty = std::getenv("SSH_TTY");
    if (sshTty && std::string(sshTty).rfind("/dev/", 0) == 0) {
      _exit(0);
    }
    _exit(1);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  close(masterFd);

  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
#endif
}
