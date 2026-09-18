#include <cstdint>

#include "ETerminal.pb.h"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "ServerFifoPath.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"
#ifdef WIN32
#include "PseudoUserTerminalWindows.hpp"
#endif
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

void UserTerminalHandler::runConPtyTerminal(PseudoUserTerminal& conpty) {
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
    const bool routerReadable = pendingInput.length() < maxPendingInput &&
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
#endif
