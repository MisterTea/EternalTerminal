#include "HtmClient.hpp"

#include "ControlMode.hpp"
#include "RawSocketUtils.hpp"

#ifdef WIN32
#include <windows.h>
#endif

namespace et {
namespace {
void writeHtmStdout(const char* buf, size_t n) {
#ifdef WIN32
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, static_cast<DWORD>(n),
            &written, NULL);
#else
  RawSocketUtils::writeAll(STDOUT_FILENO, buf, n);
#endif
}

void writeDcs() { writeHtmStdout(kControlModeDcs, strlen(kControlModeDcs)); }
}  // namespace

HtmClient::HtmClient(shared_ptr<SocketHandler> _socketHandler,
                     const SocketEndpoint& endpoint)
    : IpcPairClient(_socketHandler, endpoint) {}

#ifdef WIN32
void HtmClient::run() {
  writeDcs();
  const int BUF_SIZE = 1024;
  char buf[BUF_SIZE];
  HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);

  while (true) {
    bool didWork = false;
    if (WaitForSingleObject(stdinHandle, 0) == WAIT_OBJECT_0) {
      DWORD n = 0;
      if (!ReadFile(stdinHandle, buf, BUF_SIZE, &n, NULL)) {
        throw std::runtime_error("Cannot read from stdin");
      }
      if (n == 0) {
        throw std::runtime_error("stdin has closed abruptly.");
      }
      socketHandler->writeAllOrThrow(endpointFd, buf, static_cast<int>(n),
                                     false);
      didWork = true;
    }

    if (socketHandler->hasData(endpointFd)) {
      int rc = socketHandler->read(endpointFd, buf, BUF_SIZE);
      if (rc < 0) {
        throw std::runtime_error("Cannot read from raw socket");
      }
      if (rc == 0) {
        LOG(INFO) << "htmd has closed";
        endpointFd = -1;
        return;
      }
      writeHtmStdout(buf, static_cast<size_t>(rc));
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
