#include "HtmClient.hpp"

#include "HtmHeaderCodes.hpp"
#include "HtmServer.hpp"
#include "IpcPairClient.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
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
void HtmClient::run() {
  const int BUF_SIZE = 1024;
  char buf[BUF_SIZE];
  while (true) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(endpointFd, &rfd);
    FD_SET(STDIN_FILENO, &rfd);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(max(STDIN_FILENO, endpointFd) + 1, &rfd, NULL, NULL, &tv);

    if (FD_ISSET(STDIN_FILENO, &rfd)) {
      VLOG(1) << "STDIN -> " << endpointFd;
      int rc = ::read(STDIN_FILENO, buf, BUF_SIZE);
      if (rc < 0) {
        throw std::runtime_error("Cannot read from raw socket");
      }
      if (rc == 0) {
        throw std::runtime_error("stdin has closed abruptly.");
      }
      socketHandler->writeAllOrThrow(endpointFd, buf, rc, false);
    }

    if (FD_ISSET(endpointFd, &rfd)) {
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

      // HACK: In the future we should use heartbeats to detect a dead server.
      // For now, just listen for session end. SESSION_END is a 1-byte packet
      // and may arrive alone or at the front of a coalesced read.
      if (rc >= 1 && buf[0] == SESSION_END) {
        LOG(INFO) << "htmd has closed";
        endpointFd = -1;
        return;
      }

      writeHtmStdout(buf, static_cast<size_t>(rc));
    }
  }
}
#endif
}  // namespace et
