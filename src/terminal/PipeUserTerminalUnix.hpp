#ifndef __PIPE_USER_TERMINAL_UNIX_HPP__
#define __PIPE_USER_TERMINAL_UNIX_HPP__

#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "UserTerminal.hpp"

namespace et {
/**
 * @brief Runs a remote command on pipes (stdin/stdout/stderr) without a pty.
 *
 * Used for `et -T -c ...`: binary stdio, separate stderr, no login shell
 * motd/echo, and no `; exit` typed into a shell.
 */
class PipeUserTerminal : public UserTerminal {
 public:
  explicit PipeUserTerminal(const string& command)
      : command(command),
        pid(-1),
        stdinWriteFd(-1),
        stdoutReadFd(-1),
        stderrReadFd(-1) {}

  virtual ~PipeUserTerminal() { cleanup(); }

  virtual int setup(int routerFd) {
    int stdinPipe[2];
    int stdoutPipe[2];
    int stderrPipe[2];
    int readyPipe[2];
    FATAL_FAIL(pipe(stdinPipe));
    FATAL_FAIL(pipe(stdoutPipe));
    FATAL_FAIL(pipe(stderrPipe));
    FATAL_FAIL(pipe(readyPipe));

    pid = fork();
    switch (pid) {
      case -1:
        FATAL_FAIL(pid);
        break;
      case 0: {
        // Child: close parent-only ends before dup2.
        if (routerFd >= 0) {
          close(routerFd);
        }
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stderrPipe[0]);
        close(readyPipe[0]);
        FATAL_FAIL(dup2(stdinPipe[0], STDIN_FILENO));
        FATAL_FAIL(dup2(stdoutPipe[1], STDOUT_FILENO));
        FATAL_FAIL(dup2(stderrPipe[1], STDERR_FILENO));
        close(stdinPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[1]);

        // Signal parent that stdout/stderr write ends are held via dup2 so it
        // may close its copies without racing a premature EOF on the readers.
        // Do this before getpwuid: a multithreaded fork can deadlock there.
        {
          char ready = 1;
          FATAL_FAIL(::write(readyPipe[1], &ready, 1));
          close(readyPipe[1]);
        }

        passwd* pwd = getpwuid(getuid());
        if (pwd && pwd->pw_dir) {
          chdir(pwd->pw_dir);
        }
        setenv("ET_VERSION", ET_VERSION, 1);
        signal(SIGCHLD, SIG_DFL);

        // Pin /bin/sh: predictable across CI (dash/FreeBSD sh/bash), no login
        // motd from $SHELL. POSIX octal printf escapes work in all of these.
        const char* shell = "/bin/sh";
        VLOG(1) << "Child process launching pipe command via " << shell
                << " -c " << command;
        execl(shell, shell, "-c", command.c_str(), (char*)NULL);
        // execl failed
        _exit(127);
      }
      default: {
        close(stdinPipe[0]);
        // Close ready write end so read returns if the child dies early.
        close(readyPipe[1]);
        // Wait until the child has dup2'd the write ends onto stdout/stderr
        // before closing our copies; otherwise readers can see EOF while the
        // child still holds only the pre-dup2 fds (or none).
        {
          char ready = 0;
          ssize_t n = ::read(readyPipe[0], &ready, 1);
          if (n != 1) {
            LOG(ERROR) << "PipeUserTerminal ready handshake failed";
          }
          close(readyPipe[0]);
        }
        close(stdoutPipe[1]);
        close(stderrPipe[1]);
        stdinWriteFd = stdinPipe[1];
        stdoutReadFd = stdoutPipe[0];
        stderrReadFd = stderrPipe[0];
      }
    }

    auto setNonBlocking = [](int fd) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }
    };
    setNonBlocking(stdinWriteFd);
    setNonBlocking(stdoutReadFd);
    setNonBlocking(stderrReadFd);
    return stdoutReadFd;
  }

  virtual void runTerminal() {}

  virtual void cleanup() {
    auto closeFd = [](int& fd) {
      if (fd >= 0) {
        close(fd);
        fd = -1;
      }
    };
    closeFd(stdinWriteFd);
    closeFd(stdoutReadFd);
    closeFd(stderrReadFd);
  }

  virtual void handleSessionEnd() {
#if __NetBSD__
    int throwaway;
    if (pid > 0) {
      FATAL_FAIL(waitpid(pid, &throwaway, WUNTRACED));
    }
#else
    siginfo_t childInfo;
    if (pid > 0) {
      if (waitid(P_PID, pid, &childInfo, WEXITED) == -1) {
        LOG(ERROR) << "waitid failed, child already reaped.";
      }
    }
#endif
  }

  virtual void setInfo(const winsize& /*tmpwin*/) {
    // No pty; window size does not apply.
  }

  virtual int getFd() { return stdoutReadFd; }
  virtual int getInputFd() { return stdinWriteFd; }
  virtual int getStderrFd() { return stderrReadFd; }

  pid_t getPid() { return pid; }

 protected:
  string command;
  pid_t pid;
  int stdinWriteFd;
  int stdoutReadFd;
  int stderrReadFd;
};
}  // namespace et

#endif
