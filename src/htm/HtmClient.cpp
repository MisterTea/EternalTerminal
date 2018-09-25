#include "HtmClient.hpp"

#include "HtmServer.hpp"
#include "IpcPairClient.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "RawSocketUtils.hpp"

namespace et {
HtmClient::HtmClient(shared_ptr<SocketHandler> _socketHandler,
                     const SocketEndpoint& endpoint)
    : IpcPairClient(_socketHandler, endpoint) {}

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
      VLOG(1) << endpointFd << " -> STDOUT";
      int rc = socketHandler->read(endpointFd, buf, BUF_SIZE);
      if (rc < 0) {
        throw std::runtime_error("Cannot read from raw socket");
      }
      if (rc == 0) {
        throw std::runtime_error("htmd has closed abruptly.");
      }
      RawSocketUtils::writeAll(STDOUT_FILENO, buf, rc);
    }
  }
}
}  // namespace et
