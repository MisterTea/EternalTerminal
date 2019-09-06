#ifndef __PSUEDO_USER_TERMINAL_HPP__
#define __PSUEDO_USER_TERMINAL_HPP__

#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#if __APPLE__
#include <sys/ucred.h>
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#elif __NetBSD__  // do not need pty.h on NetBSD
#else
#include <pty.h>
#endif

#ifdef WITH_UTEMPTER
#include <utempter.h>
#endif

#include "UserTerminal.hpp"

namespace et {
class PsuedoUserTerminal : public UserTerminal {
 public:
  virtual ~PsuedoUserTerminal() {}

  virtual int setup(int routerFd) {
    pid_t pid = forkpty(&masterFd, NULL, NULL, NULL);
    switch (pid) {
      case -1:
        FATAL_FAIL(pid);
      case 0: {
        close(routerFd);
        runTerminal();
        exit(0);
      }
      default: {
        // parent
        break;
      }
    }

#ifdef WITH_UTEMPTER
    {
      char buf[1024];
      sprintf(buf, "et [%lld]", (long long)getpid());
      utempter_add_record(masterFd, buf);
    }
#endif
    return masterFd;
  }

  virtual void runTerminal() {
    passwd* pwd = getpwuid(getuid());
    chdir(pwd->pw_dir);
    string terminal = string(::getenv("SHELL"));
    VLOG(1) << "Child process launching terminal " << terminal;
    setenv("ET_VERSION", ET_VERSION, 1);
    FATAL_FAIL(execl(terminal.c_str(), terminal.c_str(), "--login", NULL));
  }

  virtual void cleanup() {
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
  }

  virtual void handleSessionEnd() {
#if __NetBSD__  // this unfortunateness seems to be fixed in NetBSD-8 (or at
                // least -CURRENT) sadness for now :/
    int throwaway;
    FATAL_FAIL(waitpid(getPid(), &throwaway, WUNTRACED));
#else
    siginfo_t childInfo;
    FATAL_FAIL(waitid(P_PID, getPid(), &childInfo, WEXITED));
#endif
  }

  virtual void setInfo(const winsize& tmpwin) {
    ioctl(masterFd, TIOCSWINSZ, &tmpwin);
  }

  pid_t getPid() { return pid; }

  virtual int getFd() { return masterFd; }

 protected:
  pid_t pid;
  int masterFd;
};
}  // namespace et

#endif
