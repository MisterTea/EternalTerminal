#include "UserTerminalHandler.hpp"

#include <cstdint>

#include "ETerminal.pb.h"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "ServerFifoPath.hpp"
#include "UserTerminalRouter.hpp"

#ifdef WIN32
#include "PseudoUserTerminal.hpp"
#endif

namespace et {
#ifdef WIN32
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
  tui.set_uid(0);
  tui.set_gid(0);

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
      // _putenv updates both the CRT environment (so getenv() in this
      // process observes it) and the OS environment block.
      string entry = ti.environmentnames(a) + "=" + ti.environmentvalues(a);
      _putenv(entry.c_str());
    }
    socketHandler->minimizeKernelBuffering(routerFd);
    break;
  }

  // ConPTY terminal ignores the router fd for I/O; output is drained via
  // PseudoUserTerminal::drainOutput() and input via writeInput().
  term->setup(routerFd);
  // Apply the initial size if the client sent one before first output.
  runUserTerminal(0);
  socketHandler->close(routerFd);
}

void UserTerminalHandler::runUserTerminal(int masterFd) {
  auto* conpty = dynamic_cast<PseudoUserTerminal*>(term.get());
  if (conpty) {
    runConPtyTerminal(*conpty);
    return;
  }
  // Test doubles (e.g. FakeUserTerminal) expose a socket fd instead of a
  // ConPTY. Pump it with the same wire protocol as Unix: raw terminal output
  // bytes to the router, packet-framed input from the router.
  runSocketTerminal(masterFd);
}

void UserTerminalHandler::runConPtyTerminal(
    PseudoUserTerminal& conpty) {
  while (true) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }
    try {
      // Router -> ConPTY input (packet-framed, same wire protocol as Unix).
      if (socketHandler->hasData(routerFd)) {
        char packetType = 0;
        ssize_t rc =
            socketHandler->read(routerFd, &packetType, sizeof(packetType));
        if (rc == 0) {
          throw std::runtime_error(
              "Router has ended abruptly.  Killing terminal session.");
        }
        if (rc < 0) {
          int err = GetErrno();
          if (err != EAGAIN && err != EWOULDBLOCK) {
            throw std::runtime_error(string("Router read error: ") +
                                     strerror(err));
          }
        } else if (rc > 0) {
          switch (packetType) {
            case TERMINAL_BUFFER: {
              TerminalBuffer tb =
                  socketHandler->readProto<TerminalBuffer>(routerFd, false);
              conpty.writeInput(tb.buffer());
              break;
            }
            case TERMINAL_INFO: {
              TerminalInfo ti =
                  socketHandler->readProto<TerminalInfo>(routerFd, false);
              winsize tmpwin;
              tmpwin.ws_row = static_cast<unsigned short>(ti.row());
              tmpwin.ws_col = static_cast<unsigned short>(ti.column());
              tmpwin.ws_xpixel = static_cast<unsigned short>(ti.width());
              tmpwin.ws_ypixel = static_cast<unsigned short>(ti.height());
              term->setInfo(tmpwin);
              break;
            }
            default:
              break;
          }
        }
      }

      // ConPTY -> router output as raw bytes (matches Unix handler).
      string output = conpty.drainOutput();
      if (!output.empty()) {
        socketHandler->writeAllOrThrow(routerFd, output.data(), output.size(),
                                      false);
      }

      if (!conpty.isRunning()) {
        // Drain any final output before exiting.
        string tail = conpty.drainOutput();
        if (!tail.empty()) {
          socketHandler->writeAllOrThrow(routerFd, tail.data(), tail.size(),
                                        false);
        }
        LOG(INFO) << "Terminal session ended";
        term->handleSessionEnd();
        lock_guard<recursive_mutex> guard(shutdownMutex);
        shuttingDown = true;
        break;
      }
    } catch (const std::exception& ex) {
      LOG(INFO) << ex.what();
      lock_guard<recursive_mutex> guard(shutdownMutex);
      shuttingDown = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  term->cleanup();
}

void UserTerminalHandler::runSocketTerminal(int masterFd) {
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  string pendingInput;
  const size_t maxPendingInput = 256 * 1024;

  while (true) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }
    // Only read terminal output when the router can accept it, so
    // backpressure reaches the shell instead of killing the session.
    // (WSAPoll-based checks: no FD_SETSIZE ceiling to worry about.)
    const bool routerWritable = isSocketWritable(routerFd);
    const bool termReadable =
        routerWritable && socketHandler->hasData(masterFd);
    const bool routerReadable =
        pendingInput.length() < maxPendingInput &&
        socketHandler->hasData(routerFd);
    if (!termReadable && !routerReadable && pendingInput.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    VLOG(4) << "socket poll is done";

    try {
      if (termReadable) {
        memset(b, 0, BUF_SIZE);
        ssize_t rc = socketHandler->read(masterFd, b, BUF_SIZE);
        int readErrno = GetErrno();
        if (rc > 0) {
          VLOG(4) << "Read from terminal";
          string s(b, rc);
          socketHandler->writeAllOrThrow(routerFd, b, rc, false);
          VLOG(4) << "Write to client";
        } else if (rc == 0) {
          LOG(INFO) << "Terminal session ended";
          term->handleSessionEnd();
          lock_guard<recursive_mutex> guard(shutdownMutex);
          shuttingDown = true;
          break;
        } else if (readErrno == EAGAIN || readErrno == EWOULDBLOCK) {
          LOG(INFO) << "Terminal read temporarily unavailable, retrying...";
          continue;
        } else {
          LOG(ERROR) << "Terminal read error: " << readErrno << " "
                     << strerror(readErrno);
          term->handleSessionEnd();
          lock_guard<recursive_mutex> guard(shutdownMutex);
          shuttingDown = true;
          break;
        }
      }

      if (routerReadable) {
        char packetType = 0;
        ssize_t rc = socketHandler->read(routerFd, &packetType, 1);
        int readErrno = GetErrno();
        if (rc < 0) {
          if (readErrno == EAGAIN || readErrno == EWOULDBLOCK ||
              readErrno == EINTR) {
            continue;
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
            pendingInput.append(tb.buffer());
            break;
          }
          case TERMINAL_INFO: {
            TerminalInfo ti =
                socketHandler->readProto<TerminalInfo>(routerFd, false);
            winsize tmpwin;
            tmpwin.ws_row = static_cast<unsigned short>(ti.row());
            tmpwin.ws_col = static_cast<unsigned short>(ti.column());
            tmpwin.ws_xpixel = static_cast<unsigned short>(ti.width());
            tmpwin.ws_ypixel = static_cast<unsigned short>(ti.height());
            term->setInfo(tmpwin);
            break;
          }
          default:
            break;
        }
      }

      if (!pendingInput.empty()) {
        ssize_t rc = socketHandler->write(masterFd, pendingInput.data(),
                                          pendingInput.length());
        int writeErrno = GetErrno();
        if (rc > 0) {
          pendingInput.erase(0, rc);
        } else if (rc < 0 && writeErrno != EAGAIN &&
                   writeErrno != EWOULDBLOCK) {
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
#else
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
    // Shrink the etterminal->etserver unix socket send buffer so a Ctrl+C
    // flush of the server WriteBuffer is not followed by ~200KB of local
    // backlog.
    socketHandler->minimizeKernelBuffering(routerFd);
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
