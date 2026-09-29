#include "ControlMode.hpp"
#include "HtmClient.hpp"
#include "PlatformUtils.hpp"

namespace et {
namespace {
void writeDcs() {
  WriteToStdStream(STDOUT_FILENO, kControlModeDcs, strlen(kControlModeDcs));
}
}  // namespace

void drainHtmStdin() {
  int flags = fcntl(STDIN_FILENO, F_GETFL);
  if (flags >= 0) {
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
  }
  char buf[4096];
  int idle = 0;
  while (idle < 20) {
    ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n > 0) {
      idle = 0;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    idle++;
    ::usleep(5000);
  }
  // ``htm; exec $SHELL`` inherits this fd. Leave it blocking so the wrapping
  // shell can read; leftover control bytes have already been discarded.
  if (flags >= 0) {
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
  }
}

namespace {
class NonBlockingFd {
 public:
  explicit NonBlockingFd(int fd) : fd(fd), flags(fcntl(fd, F_GETFL)) {
    if (flags >= 0) {
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  }
  ~NonBlockingFd() {
    // Leave O_NONBLOCK. Restoring blocking stdout after htmd closes can
    // stall before the wrapping `htm; exec $SHELL` resumes.
  }

 private:
  int fd;
  int flags;
};
}  // namespace

void HtmClient::run() {
  writeDcs();
  {
    string path = GetTempDirectory() + "htm." + GetHtmIpcUser() + ".client.pid";
    int pidFd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (pidFd >= 0) {
      char buf[32];
      int n = snprintf(buf, sizeof(buf), "%d\n", static_cast<int>(::getpid()));
      if (n > 0) {
        ::write(pidFd, buf, static_cast<size_t>(n));
      }
      ::close(pidFd);
    }
  }
  const int BUF_SIZE = 1024;
  const size_t MAX_STDOUT_QUEUE = 256 * 1024;
  const size_t MAX_IPC_OUT_QUEUE = 256 * 1024;
  char buf[BUF_SIZE];
  string stdoutQueue;
  string ipcOutQueue;
  bool endpointOpen = true;
  string stdinAcc;
  NonBlockingFd nonBlockingStdin(STDIN_FILENO);
  NonBlockingFd nonBlockingStdout(STDOUT_FILENO);

  auto trimLine = [](string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }
    return line;
  };
  auto hasExitLine = [&](const string& data) {
    size_t pos = 0;
    while (pos < data.size()) {
      size_t nl = data.find('\n', pos);
      if (nl == string::npos) {
        break;
      }
      if (trimLine(data.substr(pos, nl - pos)) == "%exit") {
        return true;
      }
      pos = nl + 1;
    }
    return false;
  };
  auto isClientDetachCommand = [&](string line) {
    line = trimLine(line);
    if (line.empty()) {
      return false;
    }
    size_t space = line.find(' ');
    string name = space == string::npos ? line : line.substr(0, space);
    return name == "detach-client" || name == "detach" || name == "exit";
  };
  auto stdinHasDetach = [&](const string& data) {
    size_t pos = 0;
    while (pos < data.size()) {
      size_t nl = data.find_first_of("\r\n", pos);
      if (nl == string::npos) {
        break;
      }
      if (isClientDetachCommand(data.substr(pos, nl - pos))) {
        return true;
      }
      if (data[nl] == '\r' && nl + 1 < data.size() && data[nl + 1] == '\n') {
        pos = nl + 2;
      } else {
        pos = nl + 1;
      }
    }
    return isClientDetachCommand(data.substr(pos));
  };

  auto flushFd = [](int fd, string* queue) {
    while (!queue->empty()) {
      ssize_t rc = ::write(fd, queue->data(), queue->size());
      if (rc > 0) {
        queue->erase(0, static_cast<size_t>(rc));
        continue;
      }
      if (rc < 0) {
        auto localErrno = GetErrno();
        if (localErrno == EAGAIN || localErrno == EWOULDBLOCK ||
            localErrno == EINTR || localErrno == ETIMEDOUT) {
          return;
        }
        throw std::runtime_error("Cannot write to socket");
      }
      return;
    }
  };

  auto finishFromServerClose = [&]() {
    int flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (flags >= 0) {
      fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    flushFd(STDOUT_FILENO, &stdoutQueue);
    const char st[] = {'\x1b', '\\'};
    ::write(STDOUT_FILENO, st, 2);
    // Real PTY clients must not unwind: close(2) of the AF_UNIX peer and
    // easylogging atexit can block. Unit tests use pipes (isatty false).
    if (::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO)) {
      // Eat in-flight control lines (e.g. kill-pane) so ``exec $SHELL`` does
      // not run them as commands. _exit skips destructors, so restore the
      // blocking flags the wrapper expects.
      drainHtmStdin();
      int outFlags = fcntl(STDOUT_FILENO, F_GETFL);
      if (outFlags >= 0) {
        fcntl(STDOUT_FILENO, F_SETFL, outFlags & ~O_NONBLOCK);
      }
      ::_exit(0);
    }
    endpointFd = -1;
    endpointOpen = false;
  };

  while (true) {
    fd_set rfd;
    fd_set wfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_ZERO(&wfd);
    // Always watch the server socket while it is open. If we only select
    // it when stdoutQueue has room, a terminal that stops consuming DCS
    // (force-quit) fills the queue and we never observe EOF after detach.
    if (endpointOpen) {
      FD_SET(endpointFd, &rfd);
    }
    if (endpointOpen && ipcOutQueue.size() < MAX_IPC_OUT_QUEUE) {
      FD_SET(STDIN_FILENO, &rfd);
    }
    int maxFd = endpointOpen ? max(STDIN_FILENO, endpointFd) : STDOUT_FILENO;
    if (!stdoutQueue.empty()) {
      FD_SET(STDOUT_FILENO, &wfd);
      maxFd = max(maxFd, STDOUT_FILENO);
    }
    if (endpointOpen && !ipcOutQueue.empty()) {
      FD_SET(endpointFd, &wfd);
    }
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    int nsel =
        select(maxFd + 1, &rfd,
               (!stdoutQueue.empty() || !ipcOutQueue.empty()) ? &wfd : NULL,
               NULL, &tv);
    if (nsel < 0) {
      auto localErrno = GetErrno();
      if (localErrno != EINTR) {
        throw std::runtime_error("select failed");
      }
      continue;
    }

    if (endpointOpen && ipcOutQueue.size() < MAX_IPC_OUT_QUEUE &&
        FD_ISSET(STDIN_FILENO, &rfd)) {
      int rc = ::read(STDIN_FILENO, buf, BUF_SIZE);
      if (rc < 0) {
        auto localErrno = GetErrno();
        if (localErrno != EAGAIN && localErrno != EWOULDBLOCK &&
            localErrno != EINTR) {
          throw std::runtime_error("Cannot read from raw socket");
        }
      } else if (rc == 0) {
        throw std::runtime_error("stdin has closed abruptly.");
      } else {
        string chunk(buf, static_cast<size_t>(rc));
        ipcOutQueue.append(chunk);
        stdinAcc.append(chunk);
        if (stdinHasDetach(stdinAcc)) {
          try {
            flushFd(endpointFd, &ipcOutQueue);
          } catch (const std::exception&) {
          }
          finishFromServerClose();
          return;
        }
      }
    }

    if (endpointOpen) {
      struct pollfd pfd;
      pfd.fd = endpointFd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      int pr = ::poll(&pfd, 1, 0);
      const bool hungup =
          pr >= 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
      const bool readable = (pr >= 0 && (pfd.revents & POLLIN) != 0) ||
                            FD_ISSET(endpointFd, &rfd);
      if (readable) {
        int rc = ::read(endpointFd, buf, BUF_SIZE);
        if (rc < 0) {
          auto localErrno = GetErrno();
          if (localErrno != EAGAIN && localErrno != EWOULDBLOCK &&
              localErrno != EINTR) {
            finishFromServerClose();
            return;
          }
        } else if (rc == 0) {
          finishFromServerClose();
          return;
        } else {
          string chunk(buf, static_cast<size_t>(rc));
          if (stdoutQueue.size() < MAX_STDOUT_QUEUE) {
            stdoutQueue.append(chunk);
          } else if (hasExitLine(chunk)) {
            stdoutQueue.append("%exit\n");
          }
          if (hasExitLine(stdoutQueue) || hasExitLine(chunk)) {
            finishFromServerClose();
            return;
          }
        }
      }
      if (hungup) {
        finishFromServerClose();
        return;
      }
    }

    if (endpointOpen && !ipcOutQueue.empty()) {
      try {
        flushFd(endpointFd, &ipcOutQueue);
      } catch (const std::exception&) {
        finishFromServerClose();
        return;
      }
    }
    if (!stdoutQueue.empty()) {
      flushFd(STDOUT_FILENO, &stdoutQueue);
    }
    if (!endpointOpen && stdoutQueue.empty()) {
      return;
    }
  }
}
}  // namespace et
