#include "LogHandler.hpp"
#include "SubprocessUtils.hpp"

namespace et {
string SubprocessUtils::SubprocessToStringInteractive(
    const string& command, const vector<string>& args) {
  int stdout_pipe[2];
  int stderr_pipe[2];
  char buf[4096];
  if (pipe(stdout_pipe) == -1) {
    STFATAL << "pipe";
    exit(1);
  }
  if (pipe(stderr_pipe) == -1) {
    STFATAL << "pipe";
    exit(1);
  }

  pid_t pid = fork();
  if (pid == 0) {
    // child process: keep stdout and stderr on separate pipes so the parent
    // can capture credentials without streaming them, while still showing
    // SSH_MSG_USERAUTH_BANNER text live on the parent's stderr.
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);

    // Do not inherit the client's stdin. OpenSSH would forward it to the
    // bootstrap session and consume the remote command stream (VS Code writes
    // the install script on stdin before `et` starts reading). Passphrases
    // still come from /dev/tty.
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0) {
      STFATAL << "open /dev/null: " << strerror(errno);
      exit(1);
    }
    if (dup2(devnull, STDIN_FILENO) < 0) {
      STFATAL << "dup2 /dev/null: " << strerror(errno);
      exit(1);
    }
    if (devnull != STDIN_FILENO) {
      close(devnull);
    }

    char** argsArray = new char*[args.size() + 2];
    argsArray[0] = strdup(command.c_str());
    for (int a = 0; a < args.size(); a++) {
      argsArray[a + 1] = strdup(args[a].c_str());
    }
    argsArray[args.size() + 1] = NULL;
    execvp(command.c_str(), argsArray);

    LOG(INFO) << "execvp error";
    for (int a = 0; a <= args.size(); a++) {
      free(argsArray[a]);
    }
    delete[] argsArray;
    exit(1);
  } else if (pid > 0) {
    // parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    string stdoutBuffer;
    bool stdoutOpen = true;
    bool stderrOpen = true;
    while (stdoutOpen || stderrOpen) {
      struct pollfd fds[2];
      nfds_t nfds = 0;
      int stdoutIdx = -1;
      int stderrIdx = -1;
      if (stdoutOpen) {
        stdoutIdx = static_cast<int>(nfds);
        fds[nfds].fd = stdout_pipe[0];
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
      }
      if (stderrOpen) {
        stderrIdx = static_cast<int>(nfds);
        fds[nfds].fd = stderr_pipe[0];
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
      }

      int ready = poll(fds, nfds, -1);
      if (ready == -1) {
        if (errno == EINTR) {
          continue;
        }
        STFATAL << "poll";
        exit(1);
      }

      auto drainFd = [&](int fd, bool* openFlag, bool isStderr) {
        while (true) {
          int nbytes = read(fd, buf, sizeof(buf));
          if (nbytes < 0) {
            if (errno == EINTR) {
              continue;
            }
            *openFlag = false;
            return;
          }
          if (nbytes == 0) {
            *openFlag = false;
            return;
          }
          if (isStderr) {
            // Redirected stderr: the helper writes the saved terminal and the
            // log. Otherwise stderr is still the terminal, so write it here.
            if (!LogHandler::forwardSubprocessStderr(
                    buf, static_cast<size_t>(nbytes))) {
              const char* cursor = buf;
              size_t remaining = static_cast<size_t>(nbytes);
              while (remaining > 0) {
                ssize_t written = write(STDERR_FILENO, cursor, remaining);
                if (written < 0) {
                  if (errno == EINTR) {
                    continue;
                  }
                  // Best-effort live tee; keep draining so the child cannot
                  // deadlock even if the parent's stderr is unavailable.
                  break;
                }
                cursor += written;
                remaining -= static_cast<size_t>(written);
              }
            }
          } else {
            stdoutBuffer.append(buf, static_cast<size_t>(nbytes));
          }
          // Read once per poll readiness notification; additional data will
          // wake poll again. Avoid busy-spinning on a non-blocking fd.
          return;
        }
      };

      if (stdoutIdx >= 0 &&
          (fds[stdoutIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
        drainFd(stdout_pipe[0], &stdoutOpen, false);
      }
      if (stderrIdx >= 0 &&
          (fds[stderrIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
        drainFd(stderr_pipe[0], &stderrOpen, true);
      }
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    return stdoutBuffer;
  } else {
    LOG(INFO) << "Failed to fork";
    exit(1);
  }
}
}  // namespace et
