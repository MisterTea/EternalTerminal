#ifndef __PSUEDO_USER_TERMINAL_UNIX_HPP__
#define __PSUEDO_USER_TERMINAL_UNIX_HPP__

#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <chrono>
#include <thread>

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
/**
 * @brief Forks a pseudo-terminal, runs the user's shell, and proxies the fd.
 */
class PseudoUserTerminal : public UserTerminal {
 public:
  PseudoUserTerminal()
      : pid(-1), processGroupId(-1), masterFd(-1), childReaped(false) {}

  virtual ~PseudoUserTerminal() {}

  virtual int setup(int routerFd) {
    pid = forkpty(&masterFd, NULL, NULL, NULL);
    switch (pid) {
      case -1:
        FATAL_FAIL(pid);
        break;
      case 0: {
        close(routerFd);
        runTerminal();
        // only get here if execl fails so a break is not needed since we exit
        exit(0);
      }
      default: {
        // parent
        // forkpty creates a session/process group led by the child shell.
        // Keep the original identity so later termination never targets a
        // group inferred from a potentially reused PID.
        processGroupId = pid;
        childReaped = false;
      }
    }

#ifdef WITH_UTEMPTER
    {
      char buf[1024];
      sprintf(buf, "etterminal [%lld]", (long long)getpid());
      utempter_add_record(masterFd, buf);
    }
#endif

    // The handler polls this fd with select() and does non-blocking reads and
    // writes, so make the master non-blocking here, where it is created.  If it
    // stayed blocking, a large input burst would block the handler's single
    // write() call and deadlock against the shell's echo (see
    // UserTerminal::setup and UserTerminalHandler::runUserTerminal).
    int flags = fcntl(masterFd, F_GETFL, 0);
    if (flags != -1) {
      fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
    }
    return masterFd;
  }

  /**
   * @brief Executes the login shell after setting up the PTY child process.
   */
  virtual void runTerminal() {
    passwd* pwd = getpwuid(getuid());
    chdir(pwd->pw_dir);
    string terminal = string(::getenv("SHELL"));
    VLOG(1) << "Child process launching terminal " << terminal;
    setenv("ET_VERSION", ET_VERSION, 1);
    // bash will not reset SIGCHLD to SIG_DFL when run, remembering the current
    // SIGCHLD disposition as the "original value" and allowing the user to
    // "reset" the signal handler to it's "original value" (trap --help).
    //
    // If our current SIGCHLD is SIG_IGN then it will be impossible, from
    // within bash, to set it to SIG_DFL by issuing "trap -- - SIGCHLD". This
    // in turn means that innocent implementations assuming they receive
    // SIGCHLD without anything special required on their part, break.
    // An example is Python2's popen(), which will fail with
    // "IOError: [Errno 10] No child processes".
    //
    // Such processes *could* help themselves by setting SIGCHLD to SIG_DFL
    // from within the process, but this is an esoteric requirement from the
    // process and many don't. And as mentioned, the shell user can't help
    // with "trap -- - SIGCHLD" either.
    //
    // Let's help everyone by setting SIGCHLD to SIG_DFL here, right before
    // exec'ing the shell. By doing it here, and not somewhere before, we add
    // no requirements for any wait(2) on our part.
    //
    signal(SIGCHLD, SIG_DFL);
    FATAL_FAIL(execl(terminal.c_str(), terminal.c_str(), "-l", NULL));
  }

  /** @brief Removes any temporary PTY bookkeeping (utempter). */
  virtual void cleanup() {
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
  }

  /** @brief Waits for the child shell to exit before returning. */
  virtual void handleSessionEnd() {
    if (getPid() <= 0 || childReaped) {
      return;
    }
#if __NetBSD__  // this unfortunateness seems to be fixed in NetBSD-8 (or at
                // least -CURRENT) sadness for now :/
    int throwaway;
    pid_t waitResult;
    do {
      waitResult = waitpid(getPid(), &throwaway, 0);
    } while (waitResult == -1 && errno == EINTR);
    if (waitResult == getPid()) {
      childReaped = true;
    } else if (waitResult == -1 && errno == ECHILD) {
      childReaped = true;
      LOG(ERROR) << "waitpid failed, child already reaped.";
    } else if (waitResult == -1) {
      LOG(ERROR) << "waitpid failed: " << strerror(errno);
    }
#else
    siginfo_t childInfo;
    int waitResult;
    do {
      waitResult = waitid(P_PID, getPid(), &childInfo, WEXITED);
    } while (waitResult == -1 && errno == EINTR);
    if (waitResult == -1) {
      const int waitErrno = errno;
      if (waitErrno == ECHILD || waitErrno == ESRCH) {
        childReaped = true;
        LOG(ERROR) << "waitid failed, child already reaped.";
      } else {
        LOG(ERROR) << "waitid failed: " << strerror(waitErrno);
      }
    } else {
      childReaped = true;
    }
#endif
  }

  virtual void terminate() {
    const pid_t childPid = getPid();
    const pid_t ownedProcessGroup = processGroupId;
    if (childPid <= 0 || childReaped || ownedProcessGroup != childPid) {
      return;
    }

    // Check the still-unreaped child before signaling.  That prevents a
    // stale terminal object from signaling a later process or process group
    // that reused the old identifiers.
    if (!childIsRunning(childPid) ||
        !ownsProcessGroup(childPid, ownedProcessGroup)) {
      return;
    }

    // Give the shell and its descendants a chance to exit cleanly first.
    // Signal the group so background descendants do not keep the session
    // alive.
    const int hupResult = ::kill(-ownedProcessGroup, SIGHUP);
    const int hupErrno = errno;
    if (hupResult == -1 && hupErrno == ESRCH) {
      return;
    }
    if (hupResult == -1) {
      LOG(ERROR) << "Could not terminate terminal process group: "
                 << strerror(hupErrno);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      if (!childIsRunning(childPid)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Recheck both identities immediately before escalation.  The child is
    // still an unreaped direct child when this succeeds, so its PID cannot
    // have been reused; the group check additionally prevents a reused group
    // id from receiving SIGKILL.
    if (!childIsRunning(childPid) ||
        !ownsProcessGroup(childPid, ownedProcessGroup)) {
      return;
    }
    const int killResult = ::kill(-ownedProcessGroup, SIGKILL);
    const int killErrno = errno;
    if (killResult == -1 && killErrno != ESRCH) {
      LOG(ERROR) << "Could not force terminate terminal process group: "
                 << strerror(killErrno);
    }
  }

  /**
   * @brief Applies terminal resize changes via `ioctl(TIOCSWINSZ)`.
   */
  virtual void setInfo(const winsize& tmpwin) {
    ioctl(masterFd, TIOCSWINSZ, &tmpwin);
  }

  pid_t getPid() { return pid; }

  virtual int getFd() { return masterFd; }

 protected:
  bool childIsRunning(pid_t childPid) {
    if (childPid <= 0 || childReaped) {
      return false;
    }
#if __NetBSD__
    int status = 0;
    pid_t waitResult;
    do {
      waitResult = waitpid(childPid, &status, WNOHANG);
    } while (waitResult == -1 && errno == EINTR);
    if (waitResult == childPid ||
        (waitResult == -1 && (errno == ECHILD || errno == ESRCH))) {
      childReaped = true;
      return false;
    }
    return waitResult == 0;
#else
    siginfo_t childInfo{};
    int waitResult;
    do {
      waitResult =
          waitid(P_PID, childPid, &childInfo, WEXITED | WNOHANG | WNOWAIT);
    } while (waitResult == -1 && errno == EINTR);
    if (waitResult == -1) {
      const int waitErrno = errno;
      if (waitErrno == ECHILD || waitErrno == ESRCH) {
        childReaped = true;
      }
      return false;
    }
    return childInfo.si_pid == 0;
#endif
  }

  bool ownsProcessGroup(pid_t childPid, pid_t ownedProcessGroup) {
    if (ownedProcessGroup <= 0 || childPid != ownedProcessGroup) {
      return false;
    }
    const pid_t currentProcessGroup = ::getpgid(childPid);
    if (currentProcessGroup == ownedProcessGroup) {
      return true;
    }
    const int groupErrno = errno;
    if (currentProcessGroup == -1 && groupErrno != ESRCH) {
      LOG(ERROR) << "Could not verify terminal process group: "
                 << strerror(groupErrno);
    }
    return false;
  }

  /** @brief PID of the child shell spawned by `forkpty`. */
  pid_t pid;
  /** @brief Process group ID captured when the child was forked. */
  pid_t processGroupId;
  /** @brief Master PTY file descriptor shared with the router. */
  int masterFd;
  /** @brief Whether the child has already been reaped. */
  bool childReaped;
};
}  // namespace et

#endif
