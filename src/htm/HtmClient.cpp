#include "HtmClient.hpp"

#include "HtmHeaderCodes.hpp"
#include "HtmServer.hpp"
#include "IpcPairClient.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "RawSocketUtils.hpp"
#include "base64.h"

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
}  // namespace

HtmClient::HtmClient(shared_ptr<SocketHandler> _socketHandler,
                     const SocketEndpoint& endpoint)
    : IpcPairClient(_socketHandler, endpoint) {}

#ifdef WIN32
void HtmClient::run() {
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
      VLOG(1) << endpointFd << " -> STDOUT (" << rc << ")";
      if (rc < 0) {
        throw std::runtime_error("Cannot read from raw socket");
      }
      if (rc == 0) {
        LOG(INFO) << "htmd has closed";
        endpointFd = -1;
        return;
      }
      if (rc >= 1 && buf[0] == SESSION_END) {
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
  const int BUF_SIZE = 1024;
  // Bound queued daemon output so a stalled Hyper PTY cannot grow forever.
  // Stop reading IPC past this point so backpressure hits htmd instead of
  // blocking this loop (blocking stdout + pending stdin deadlocks Hyper).
  const size_t MAX_STDOUT_QUEUE = 256 * 1024;
  const size_t MAX_IPC_OUT_QUEUE = 256 * 1024;
  const int32_t MAX_PACKET_LENGTH = 4 * 1024 * 1024;
  const string kInit = "\x1b[###q";
  char buf[BUF_SIZE];
  string stdoutQueue;
  string ipcOutQueue;
  string packetBuf;
  bool inHtmMode = false;
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

  auto consumeDaemonBytes = [&](const char* data, size_t n) -> bool {
    if (!inHtmMode) {
      stdoutQueue.append(data, n);
      auto pos = stdoutQueue.find(kInit);
      if (pos == string::npos) {
        return true;
      }
      inHtmMode = true;
      packetBuf = stdoutQueue.substr(pos + kInit.size());
      stdoutQueue.erase(pos + kInit.size());
    } else {
      packetBuf.append(data, n);
    }
    while (!packetBuf.empty()) {
      if (packetBuf[0] == SESSION_END) {
        return false;
      }
      if (packetBuf.size() < 9) {
        break;
      }
      int32_t length = 0;
      if (!Base64::Decode(packetBuf.data() + 1, 8,
                          reinterpret_cast<char*>(&length), 4) ||
          length < 0 || length > MAX_PACKET_LENGTH) {
        stdoutQueue.push_back(packetBuf[0]);
        packetBuf.erase(0, 1);
        continue;
      }
      if (packetBuf.size() < 9u + static_cast<size_t>(length)) {
        break;
      }
      stdoutQueue.append(packetBuf, 0, 9u + static_cast<size_t>(length));
      packetBuf.erase(0, 9u + static_cast<size_t>(length));
    }
    return true;
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
      VLOG(1) << "STDIN -> " << endpointFd;
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
      VLOG(1) << endpointFd << " -> STDOUT (" << rc << ")";
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
      } else if (!consumeDaemonBytes(buf, static_cast<size_t>(rc))) {
        LOG(INFO) << "htmd has closed";
        endpointFd = -1;
        return;
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
