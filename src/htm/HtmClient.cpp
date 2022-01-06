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

#ifdef WIN32
    DWORD numberOfEvents;
    FATAL_FAIL_IF_ZERO(GetNumberOfConsoleInputEvents(
        GetStdHandle(STD_INPUT_HANDLE), &numberOfEvents));
#endif
    if (FD_ISSET(STDIN_FILENO, &rfd)
#ifdef WIN32
        && numberOfEvents
#endif
    ) {
      VLOG(1) << "STDIN -> " << endpointFd;
#ifdef WIN32
      DWORD events;
      INPUT_RECORD buffer[128];
      HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
      PeekConsoleInput(handle, buffer, 128, &events);
      if (events > 0) {
        ReadConsoleInput(handle, buffer, 128, &events);
        string s;
        for (int keyEvent = 0; keyEvent < events; keyEvent++) {
          if (buffer[keyEvent].EventType == KEY_EVENT &&
              buffer[keyEvent].Event.KeyEvent.bKeyDown) {
            char charPressed =
                ((char)buffer[keyEvent].Event.KeyEvent.uChar.AsciiChar);
            if (charPressed) {
              s += charPressed;
            }
          }
        }
        if (s.length()) {
          socketHandler->writeAllOrThrow(endpointFd, s.c_str(), s.length(),
                                         false);
        }
      }
#else
      int rc = ::read(STDIN_FILENO, buf, BUF_SIZE);
      if (rc < 0) {
        throw std::runtime_error("Cannot read from raw socket");
      }
      if (rc == 0) {
        throw std::runtime_error("stdin has closed abruptly.");
      }
      socketHandler->writeAllOrThrow(endpointFd, buf, rc, false);
#endif
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
      // For now, just listen for session end.
      if (rc == 1 && buf[0] == SESSION_END) {
        LOG(INFO) << "htmd has closed";
        endpointFd = -1;
        return;
      }

#ifdef WIN32
      RawSocketUtils::writeAll(GetStdHandle(STD_OUTPUT_HANDLE), buf, rc);
#else
      RawSocketUtils::writeAll(STDOUT_FILENO, buf, rc);
#endif
    }
  }
}
}  // namespace et
