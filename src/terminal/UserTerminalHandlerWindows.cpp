#include <cstdint>

#include "ETerminal.pb.h"
#include "PipeUserTerminal.hpp"
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
      shuttingDown(false),
      pipeMode(false) {
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

void UserTerminalHandler::forwardOutputToRouter(const char* data, size_t length,
                                                bool isStderr) {
  if (length == 0) {
    return;
  }
  TerminalBuffer tb;
  tb.set_buffer(string(data, length));
  if (isStderr) {
    tb.set_is_stderr(true);
  }
  socketHandler->writePacket(
      routerFd, Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
}

void UserTerminalHandler::finishSession() {
  const int exitCode = term->handleSessionEnd();
  TerminalExitStatus tes;
  tes.set_exitcode(exitCode);
  try {
    socketHandler->writePacket(
        routerFd,
        Packet(TerminalPacketType::TERMINAL_EXIT_STATUS, protoToString(tes)));
  } catch (const std::exception& ex) {
    LOG(INFO) << "Failed to send terminal exit status: " << ex.what();
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
    pipeMode = ti.no_pty();
    if (pipeMode) {
      if (!ti.has_command() || ti.command().empty()) {
        STFATAL << "no_pty TermInit requires a non-empty command";
      }
      term = make_shared<PipeUserTerminal>(ti.command());
      LOG(INFO) << "Starting raw pipe command session";
    }
    socketHandler->minimizeKernelBuffering(routerFd);
    if (ti.no_shell()) {
      LOG(INFO) << "Starting idle session without a shell";
      runIdleSession();
      socketHandler->close(routerFd);
      return;
    }
    break;
  }

  int masterfd = term->setup(routerFd);
  runUserTerminal(masterfd);
  socketHandler->close(routerFd);
}

void UserTerminalHandler::runIdleSession() {
  while (true) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        return;
      }
    }
    if (!socketHandler->hasData(routerFd)) {
      Sleep(10);
      continue;
    }
    char packetType = 0;
    ssize_t rc = socketHandler->read(routerFd, &packetType, 1);
    if (rc == 0) {
      LOG(INFO) << "Idle session router closed";
      return;
    }
    if (rc < 0) {
      int err = GetErrno();
      if (err == EAGAIN || err == EWOULDBLOCK) {
        continue;
      }
      LOG(INFO) << "Idle session router read error: " << strerror(err);
      return;
    }
    switch (packetType) {
      case TERMINAL_BUFFER:
        socketHandler->readProto<TerminalBuffer>(routerFd, false);
        break;
      case TERMINAL_INFO:
        socketHandler->readProto<TerminalInfo>(routerFd, false);
        break;
      case TERMINAL_CLOSE:
        LOG(INFO) << "Idle session closed";
        return;
      default:
        LOG(INFO) << "Idle session stopping on packet " << int(packetType);
        return;
    }
  }
}

void UserTerminalHandler::runUserTerminal(int masterFd) {
  auto* conpty = dynamic_cast<PseudoUserTerminal*>(term.get());
  if (conpty && !pipeMode) {
    runConPtyTerminal(*conpty);
    return;
  }
  // Test doubles (e.g. FakeUserTerminal) expose a socket fd instead of a
  // ConPTY. Pump it with the same wire protocol as Unix: packet-framed
  // terminal output to the router, packet-framed input from the router.
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
            case TERMINAL_CLOSE: {
              lock_guard<recursive_mutex> guard(shutdownMutex);
              shuttingDown = true;
              break;
            }
            default:
              break;
          }
        }
      }

      // ConPTY -> router output as TERMINAL_BUFFER packets (matches Unix).
      string output = conpty.drainOutput();
      if (!output.empty()) {
        forwardOutputToRouter(output.data(), output.size(), false);
      }

      if (!conpty.isRunning()) {
        string tail = conpty.drainOutput();
        if (!tail.empty()) {
          forwardOutputToRouter(tail.data(), tail.size(), false);
        }
        LOG(INFO) << "Terminal session ended";
        finishSession();
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
  const int inputFd = term->getInputFd();
  int activeStderrFd = term->getStderrFd();

  while (true) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }
    const bool routerWritable = isSocketWritable(routerFd);
    const bool termReadable =
        routerWritable && waitOnSocketData(masterFd, 0, 0);
    const bool stderrReadable = routerWritable && activeStderrFd >= 0 &&
                                waitOnSocketData(activeStderrFd, 0, 0);
    const bool routerReadable = pendingInput.length() < maxPendingInput &&
                                socketHandler->hasData(routerFd);
    if (!termReadable && !stderrReadable && !routerReadable &&
        pendingInput.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    VLOG(4) << "socket poll is done";

    try {
      if (termReadable) {
        memset(b, 0, BUF_SIZE);
        ssize_t rc = ::recv(masterFd, b, BUF_SIZE, 0);
        int readErrno = GetErrno();
        if (rc > 0) {
          VLOG(4) << "Read from terminal stdout";
          forwardOutputToRouter(b, static_cast<size_t>(rc), false);
        } else if (rc == 0) {
          LOG(INFO) << "Terminal session ended";
          if (pipeMode) {
            term->closeInput();
          }
          finishSession();
          lock_guard<recursive_mutex> guard(shutdownMutex);
          shuttingDown = true;
          break;
        } else if (readErrno == EAGAIN || readErrno == EWOULDBLOCK) {
          LOG(INFO) << "Terminal read temporarily unavailable, retrying...";
          continue;
        } else {
          LOG(ERROR) << "Terminal read error: " << readErrno << " "
                     << strerror(readErrno);
          finishSession();
          lock_guard<recursive_mutex> guard(shutdownMutex);
          shuttingDown = true;
          break;
        }
      }

      if (stderrReadable) {
        memset(b, 0, BUF_SIZE);
        ssize_t rc = ::recv(activeStderrFd, b, BUF_SIZE, 0);
        int readErrno = GetErrno();
        if (rc > 0) {
          VLOG(4) << "Read from terminal stderr";
          forwardOutputToRouter(b, rc, true);
        } else if (rc == 0) {
          activeStderrFd = -1;
        } else if (readErrno != EAGAIN && readErrno != EWOULDBLOCK) {
          LOG(ERROR) << "Terminal stderr read error: " << readErrno << " "
                     << strerror(readErrno);
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
          case TERMINAL_CLOSE: {
            lock_guard<recursive_mutex> guard(shutdownMutex);
            shuttingDown = true;
            break;
          }
          default:
            break;
        }
      }

      if (!pendingInput.empty()) {
        ssize_t rc = ::send(inputFd, pendingInput.data(),
                            static_cast<int>(pendingInput.length()), 0);
        int writeErrno = GetErrno();
        if (rc > 0) {
          pendingInput.erase(0, rc);
        } else if (rc < 0 && writeErrno != EAGAIN &&
                   writeErrno != EWOULDBLOCK) {
          LOG(ERROR) << "Terminal write error: " << writeErrno << " "
                     << strerror(writeErrno);
          finishSession();
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
