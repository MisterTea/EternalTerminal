#include "HtmClient.hpp"

#include "ControlMode.hpp"
#include "RawSocketUtils.hpp"

#ifdef WIN32
#include <windows.h>

#include <algorithm>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace et {
namespace {
void writeHtmStdout(const char* buf, size_t n) {
#ifdef WIN32
  DWORD written = 0;
  const HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD consoleMode = 0;
  const auto ok =
      GetConsoleMode(stdoutHandle, &consoleMode)
          ? WriteConsoleA(stdoutHandle, buf, static_cast<DWORD>(n), &written,
                          NULL)
          : WriteFile(stdoutHandle, buf, static_cast<DWORD>(n), &written, NULL);
  if (!ok || written != n) {
    return;
  }
#else
  RawSocketUtils::writeAll(STDOUT_FILENO, buf, n);
#endif
}

#ifdef WIN32
void writeControlOutput(const char* buf, size_t n) {
  DWORD consoleMode = 0;
  if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &consoleMode)) {
    writeHtmStdout(buf, n);
    return;
  }
  // ConPTY strips tmux DCS. Carry control bytes in private CSI sequences
  // (CSI ?777;b0;b1;...q). ConPTY keeps ~16 CSI parameters; the first is
  // 777, so each sequence can hold at most 15 payload bytes.
  constexpr size_t kChunkSize = 15;
  for (size_t offset = 0; offset < n; offset += kChunkSize) {
    const size_t end = std::min(n, offset + kChunkSize);
    string encoded = "\x1b[?777";
    for (size_t i = offset; i < end; ++i) {
      encoded += ";" + to_string(static_cast<unsigned char>(buf[i]));
    }
    encoded += "q";
    writeHtmStdout(encoded.data(), encoded.size());
  }
}
#endif

void writeDcs() {
#ifdef WIN32
  writeControlOutput(kControlModeDcs, strlen(kControlModeDcs));
#else
  writeHtmStdout(kControlModeDcs, strlen(kControlModeDcs));
#endif
}
}  // namespace

HtmClient::HtmClient(shared_ptr<SocketHandler> _socketHandler,
                     const SocketEndpoint& endpoint)
    : IpcPairClient(_socketHandler, endpoint) {}

void drainHtmStdin() {
#ifdef WIN32
  return;
#else
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
#endif
}

#ifdef WIN32
void HtmClient::run() {
  const int BUF_SIZE = 1024;
  char buf[BUF_SIZE];
  HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
  DWORD consoleMode = 0;
  bool isConsole = GetConsoleMode(stdinHandle, &consoleMode) != 0;
  if (isConsole) {
    DWORD rawMode = consoleMode;
    rawMode &=
        ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    rawMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(stdinHandle, rawMode)) {
      throw std::runtime_error("Cannot put stdin in raw console mode");
    }
  }
  // ConPTY synthesizes CTRL_C_EVENT for Ctrl+C / some Ctrl+Shift chords even
  // when the GUI already handled the shortcut (e.g. Windows Terminal new-tab).
  SetConsoleCtrlHandler([](DWORD) -> BOOL { return TRUE; }, TRUE);

  auto inputStarted = std::make_shared<std::atomic_bool>(false);
  auto inputClosed = std::make_shared<std::atomic_bool>(false);
  std::thread{[handler = socketHandler, endpoint = endpointFd, stdinHandle,
               isConsole, inputStarted, inputClosed]() {
    char input[1024];
    inputStarted->store(true);
    while (true) {
      int n = 0;
      if (isConsole) {
        INPUT_RECORD record{};
        DWORD recordsRead = 0;
        if (!ReadConsoleInputW(stdinHandle, &record, 1, &recordsRead)) {
          inputClosed->store(true);
          return;
        }
        if (recordsRead != 1 || record.EventType != KEY_EVENT ||
            !record.Event.KeyEvent.bKeyDown ||
            record.Event.KeyEvent.uChar.UnicodeChar == 0) {
          continue;
        }
        const wchar_t wide = record.Event.KeyEvent.uChar.UnicodeChar;
        n = WideCharToMultiByte(CP_UTF8, 0, &wide, 1, input, sizeof(input),
                                NULL, NULL);
      } else {
        DWORD bytesRead = 0;
        if (!ReadFile(stdinHandle, input, sizeof(input), &bytesRead, NULL) ||
            bytesRead == 0) {
          inputClosed->store(true);
          return;
        }
        n = static_cast<int>(bytesRead);
      }
      try {
        handler->writeAllOrThrow(endpoint, input, n, false);
      } catch (...) {
        inputClosed->store(true);
        return;
      }
    }
  }}.detach();
  while (!inputStarted->load()) {
    std::this_thread::yield();
  }
  writeDcs();

  while (true) {
    bool didWork = false;
    if (inputClosed->load()) {
      throw std::runtime_error("stdin has closed abruptly.");
    }
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(endpointFd, &readSet);
    timeval timeout{0, 0};
    const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
    if (ready == SOCKET_ERROR) {
      throw std::runtime_error("Cannot inspect HTM socket");
    }
    if (ready > 0) {
      int rc = socketHandler->read(endpointFd, buf, BUF_SIZE);
      if (rc < 0) {
        throw std::runtime_error("Cannot read from raw socket");
      }
      if (rc == 0) {
        LOG(INFO) << "htmd has closed";
        endpointFd = -1;
        return;
      }
      writeControlOutput(buf, static_cast<size_t>(rc));
      didWork = true;
    }
    if (!didWork) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}
#else
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
#ifndef WIN32
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
#endif
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
#endif
}  // namespace et
