#ifndef WIN32
#include "UserTerminalHandler.hpp"

#include <cstdint>

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
      routerEndpoint(routerEndpoint),
      shuttingDown(false),
      ptyActive(false) {
  auto idpasskey_splited = split(idPasskey, '/');
  id = idpasskey_splited[0];
  passkey = idpasskey_splited[1];

  try {
    registerWithRouter();
  } catch (const std::runtime_error& re) {
    STFATAL << "Error connecting to router: " << re.what();
  }
}

void UserTerminalHandler::registerWithRouter() {
  TerminalUserInfo tui;
  tui.set_id(id);
  tui.set_passkey(passkey);
  tui.set_uid(getuid());
  tui.set_gid(getgid());
  tui.set_ptyactive(ptyActive);

  routerFd = ServerFifoPath::detectAndConnect(routerEndpoint, socketHandler);

  socketHandler->writePacket(
      routerFd,
      Packet(TerminalPacketType::TERMINAL_USER_INFO, protoToString(tui)));
}

int UserTerminalHandler::reconnectRouter() {
  if (routerFd >= 0) {
    // Go through the socket handler so its fd bookkeeping stays in sync;
    // a raw close() would leave a stale entry and the next connect() could
    // reuse the number and trip "fd already exists".
    socketHandler->close(routerFd);
    routerFd = -1;
  }
  LOG(INFO) << "Router connection lost; the session stays alive and waits for "
               "the router to come back.";
  int backoffSec = 1;
  while (true) {
    // Bound the shutdown latency: check the flag every second while sleeping.
    for (int a = 0; a < backoffSec; a++) {
      sleep(1);
      {
        lock_guard<recursive_mutex> guard(shutdownMutex);
        if (shuttingDown) {
          return -1;
        }
      }
    }
    try {
      registerWithRouter();
      LOG(INFO) << "Reconnected to the router; resuming the session.";
      return routerFd;
    } catch (const std::exception& re) {
      LOG(INFO) << "Router not available yet: " << re.what();
    }
    if (backoffSec < 10) {
      backoffSec *= 2;
    }
  }
}

void UserTerminalHandler::run() {
  if (!ptyActive) {
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
  }

  int masterfd = term->setup(routerFd);
  VLOG(1) << "pty opened " << masterfd;
  ptyActive = true;
  runUserTerminal(masterfd);
  socketHandler->close(routerFd);
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
          // The router (etserver) went away.  Keep the pty child alive and
          // wait for the replacement router instead of killing the session.
          routerFd = reconnectRouter();
          if (routerFd < 0) {
            break;
          }
          continue;
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
      // Router-side failure (read error, write failure, or a desynchronized
      // stream): the pty child is still fine, so wait for the router to come
      // back instead of ending the session.
      LOG(INFO) << ex.what();
      routerFd = reconnectRouter();
      if (routerFd < 0) {
        break;
      }
    }
  }

  term->cleanup();
}
}  // namespace et
#endif
