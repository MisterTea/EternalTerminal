#include "HtmClient.hpp"

#include "ControlMode.hpp"
#include "RawSocketUtils.hpp"

#ifdef WIN32
#include <algorithm>
#include <windows.h>
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
    if (flags >= 0) {
      fcntl(fd, F_SETFL, flags);
    }
  }

 private:
  int fd;
  int flags;
};
}  // namespace

void HtmClient::run() {
  writeDcs();
  const int BUF_SIZE = 1024;
  const size_t MAX_STDOUT_QUEUE = 256 * 1024;
  const size_t MAX_IPC_OUT_QUEUE = 256 * 1024;
  char buf[BUF_SIZE];
  string stdoutQueue;
  string ipcOutQueue;
  NonBlockingFd nonBlockingStdout(STDOUT_FILENO);

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

  while (true) {
    fd_set rfd;
    fd_set wfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_ZERO(&wfd);
    if (stdoutQueue.size() < MAX_STDOUT_QUEUE) {
      FD_SET(endpointFd, &rfd);
    }
    if (ipcOutQueue.size() < MAX_IPC_OUT_QUEUE) {
      FD_SET(STDIN_FILENO, &rfd);
    }
    int maxFd = max(STDIN_FILENO, endpointFd);
    if (!stdoutQueue.empty()) {
      FD_SET(STDOUT_FILENO, &wfd);
      maxFd = max(maxFd, STDOUT_FILENO);
    }
    if (!ipcOutQueue.empty()) {
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

    if (ipcOutQueue.size() < MAX_IPC_OUT_QUEUE &&
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
        ipcOutQueue.append(buf, static_cast<size_t>(rc));
      }
    }

    if (stdoutQueue.size() < MAX_STDOUT_QUEUE && FD_ISSET(endpointFd, &rfd)) {
      int rc = ::read(endpointFd, buf, BUF_SIZE);
      if (rc < 0) {
        auto localErrno = GetErrno();
        if (localErrno != EAGAIN && localErrno != EWOULDBLOCK &&
            localErrno != EINTR) {
          throw std::runtime_error("Cannot read from raw socket");
        }
      } else if (rc == 0) {
        LOG(INFO) << "htmd has closed";
        endpointFd = -1;
        return;
      } else {
        stdoutQueue.append(buf, static_cast<size_t>(rc));
      }
    }

    if (!ipcOutQueue.empty()) {
      flushFd(endpointFd, &ipcOutQueue);
    }
    if (!stdoutQueue.empty()) {
      flushFd(STDOUT_FILENO, &stdoutQueue);
    }
  }
}
#endif
}  // namespace et
