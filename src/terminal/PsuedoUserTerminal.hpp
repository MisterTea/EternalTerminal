#ifndef __PSUEDO_USER_TERMINAL_HPP__
#define __PSUEDO_USER_TERMINAL_HPP__

#include <stdlib.h>
#include "Terminal.hpp"
#if __APPLE__
#include <sys/ucred.h>
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#elif __NetBSD__  // do not need pty.h on NetBSD
#else
#include <pty.h>
#endif

namespace et {
class PsuedoUserTerminal : public Terminal {
 public:
  virtual pid_t setup(int* fd) {
    pid_t pid = forkpty(fd, NULL, NULL, NULL);
    return pid;
  }

  virtual void runTerminal() {
    passwd* pwd = getpwuid(getuid());
    chdir(pwd->pw_dir);
    string terminal = string(::getenv("SHELL"));
    VLOG(1) << "Child process launching terminal " << terminal;
    setenv("ET_VERSION", ET_VERSION, 1);
    execl(terminal.c_str(), terminal.c_str(), "--login", NULL);
    exit(0);
  }
};
}  // namespace et

#endif