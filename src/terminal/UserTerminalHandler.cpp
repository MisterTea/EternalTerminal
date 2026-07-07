#ifndef WIN32
#include "UserTerminalHandler.hpp"

#include "ETerminal.pb.h"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "ServerFifoPath.hpp"
#include "UserTerminalRouter.hpp"

namespace et {
UserTerminalHandler::UserTerminalHandler(
    shared_ptr<SocketHandler> _socketHandler, shared_ptr<UserTerminal> _term,
    bool _noratelimit, const optional<SocketEndpoint> routerEndpoint,
    const string& idPasskey)
    : socketHandler(_socketHandler),
      term(_term),
      noratelimit(_noratelimit),
      shuttingDown(false) {
  auto idpasskey_splited = split(idPasskey, '/');
  string id = idpasskey_splited[0];
  string passkey = idpasskey_splited[1];
  TerminalUserInfo tui;
  tui.set_id(id);
  tui.set_passkey(passkey);
  tui.set_uid(getuid());
  tui.set_gid(getgid());

  routerFd = ServerFifoPath::detectAndConnect(routerEndpoint, socketHandler);

  try {
    socketHandler->writePacket(
        routerFd,
        Packet(TerminalPacketType::TERMINAL_USER_INFO, protoToString(tui)));

  } catch (const std::runtime_error& re) {
    STFATAL << "Error connecting to router: " << re.what();
  }
}

void UserTerminalHandler::run() {
  while (true) {
    Packet termInitPacket;
    if (!socketHandler->readPacket(routerFd, &termInitPacket)) {
      continue;
    }
    if (termInitPacket.getHeader() != TerminalPacketType::TERMINAL_INIT) {
      STFATAL << "Invalid terminal init packet header: "
              << termInitPacket.getHeader();
    }
    TermInit ti = stringToProto<TermInit>(termInitPacket.getPayload());
    for (int a = 0; a < ti.environmentnames_size(); a++) {
      setenv(ti.environmentnames(a).c_str(), ti.environmentvalues(a).c_str(),
             true);
    }
    break;
  }

  int masterfd = term->setup(routerFd);
  VLOG(1) << "pty opened " << masterfd;
  runUserTerminal(masterfd);
  close(routerFd);
}

void UserTerminalHandler::runUserTerminal(int masterFd) {
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  time_t lastSecond = time(NULL);
  int64_t outputPerSecond = 0;

  // The pty master is non-blocking (set by UserTerminal::setup, where the fd is
  // created).  This loop is single-threaded, so we must never block inside the
  // input write: the old blocking `RawSocketUtils::writeAll(masterFd, ...)`
  // did, and a large burst of input (e.g. a pasted heredoc) echoes back, fills
  // the pty output buffer, stalls the shell, and the shell then stops reading
  // input
  // -- so the write never completes and we also stop draining output: a
  // deadlock that wedges the session past ~one pty buffer of input.  Instead we
  // buffer pending input, drain it to the pty whenever it is writable, and keep
  // reading output every iteration.  When the buffer fills we stop reading more
  // input from the router, so backpressure reaches the client.
  string pendingInput;
  const size_t maxPendingInput = 256 * 1024;

  while (true) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    fd_set wfd;
    timeval tv;

    // Only read terminal output when the router can accept it, so
    // backpressure reaches the shell instead of killing the session
    const bool routerWritable = isSocketWritable(routerFd);

    FD_ZERO(&rfd);
    FD_ZERO(&wfd);
    if (routerWritable) {
      FD_SET(masterFd, &rfd);
    }
    // Stop pulling more input from the router once the pty-input buffer is
    // full, so backpressure reaches the client instead of buffering without
    // bound.
    if (pendingInput.length() < maxPendingInput) {
      FD_SET(routerFd, &rfd);
    }
    // Wake as soon as the pty can accept more of the buffered input.
    if (!pendingInput.empty()) {
      FD_SET(masterFd, &wfd);
    }
    int maxfd = max(masterFd, routerFd);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, &wfd, NULL, &tv);
    VLOG(4) << "select is done";

    time_t currentSecond = time(NULL);
    if (lastSecond != currentSecond) {
      outputPerSecond = 0;
      lastSecond = currentSecond;
    }

    try {
      // Check for data to receive; the received
      // data includes also the data previously sent
      // on the same master descriptor (line 90).
      if (FD_ISSET(masterFd, &rfd) && (noratelimit || outputPerSecond < 1024)) {
        // Read from terminal and write to client, with a limit in rows/sec
        memset(b, 0, BUF_SIZE);
        int rc = read(masterFd, b, BUF_SIZE);
        int readErrno = errno;  // Save errno before any logging
        if (rc > 0) {
          VLOG(4) << "Read from terminal";
          string s(b, rc);
          outputPerSecond += std::count(s.begin(), s.end(), '\n');
          socketHandler->writeAllOrThrow(routerFd, b, rc, false);
          VLOG(4) << "Write to client: "
                  << std::count(s.begin(), s.end(), '\n');
        } else if (rc == 0) {
          LOG(INFO) << "Terminal session ended";
          term->handleSessionEnd();
          lock_guard<recursive_mutex> guard(shutdownMutex);
          shuttingDown = true;
          break;
        } else if (readErrno == EAGAIN || readErrno == EWOULDBLOCK) {
          // Transient error, retry
          LOG(INFO) << "Terminal read temporarily unavailable, retrying...";
          continue;
        } else {
          // Fatal read error - log with correct errno and exit gracefully
          LOG(ERROR) << "Terminal read error: " << readErrno << " "
                     << strerror(readErrno);
          term->handleSessionEnd();
          lock_guard<recursive_mutex> guard(shutdownMutex);
          shuttingDown = true;
          break;
        }
      }

      if (FD_ISSET(routerFd, &rfd)) {
        char packetType;
        int rc = read(routerFd, &packetType, 1);
        int readErrno = errno;  // Save errno before any logging
        if (rc == -1) {
          if (readErrno == EAGAIN || readErrno == EINTR) {
            continue;  // Transient error, retry
          }
          throw std::runtime_error(string("Router read error: ") +
                                   strerror(readErrno));
        }
        if (rc == 0) {
          throw std::runtime_error(
              "Router has ended abruptly.  Killing terminal session.");
        }
        switch (packetType) {
          case TERMINAL_BUFFER: {
            TerminalBuffer tb =
                socketHandler->readProto<TerminalBuffer>(routerFd, false);
            VLOG(4) << "Read from router";
            // Buffer the input; it is drained to the pty (non-blocking) below
            // so a large burst can never block this loop.
            pendingInput.append(tb.buffer());
            break;
          }
          case TERMINAL_INFO: {
            TerminalInfo ti =
                socketHandler->readProto<TerminalInfo>(routerFd, false);
            winsize tmpwin;
            tmpwin.ws_row = ti.row();
            tmpwin.ws_col = ti.column();
            tmpwin.ws_xpixel = ti.width();
            tmpwin.ws_ypixel = ti.height();
            term->setInfo(tmpwin);
            break;
          }
        }
      }

      // Drain buffered input to the pty without blocking.  A short write (the
      // pty input buffer is full) just leaves the rest pending for the next
      // iteration, so output keeps draining in the meantime.
      if (!pendingInput.empty()) {
        int rc = write(masterFd, pendingInput.data(), pendingInput.length());
        int writeErrno = errno;  // Save errno before any logging
        if (rc > 0) {
          pendingInput.erase(0, rc);
        } else if (rc < 0 && writeErrno != EAGAIN &&
                   writeErrno != EWOULDBLOCK) {
          // Fatal write error - log with correct errno and exit gracefully
          LOG(ERROR) << "Terminal write error: " << writeErrno << " "
                     << strerror(writeErrno);
          term->handleSessionEnd();
          lock_guard<recursive_mutex> guard(shutdownMutex);
          shuttingDown = true;
          break;
        }
      }
    } catch (const std::exception& ex) {
      LOG(INFO) << ex.what();
      lock_guard<recursive_mutex> guard(shutdownMutex);
      shuttingDown = true;
      break;
    }
  }

  term->cleanup();
}
}  // namespace et
#endif
