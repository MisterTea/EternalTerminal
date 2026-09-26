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
namespace {
bool handleTerminalInfo(UserTerminal& term, const TerminalInfo& ti) {
  if (ti.command() == TerminalInfo::KILL_SESSION) {
    if (ti.commandversion() != SESSION_KILL_COMMAND_VERSION) {
      LOG(WARNING) << "Ignoring unsupported terminal command version "
                   << ti.commandversion();
      return false;
    }
    term.terminate();
    term.handleSessionEnd();
    return true;
  }

  winsize tmpwin;
  tmpwin.ws_row = static_cast<unsigned short>(ti.row());
  tmpwin.ws_col = static_cast<unsigned short>(ti.column());
  tmpwin.ws_xpixel = static_cast<unsigned short>(ti.width());
  tmpwin.ws_ypixel = static_cast<unsigned short>(ti.height());
  term.setInfo(tmpwin);
  return false;
}

void writePendingOutput(const shared_ptr<SocketHandler>& socketHandler,
                        int routerFd, string& pendingOutput) {
  while (!pendingOutput.empty()) {
    const ssize_t bytesWritten = socketHandler->write(
        routerFd, pendingOutput.data(), pendingOutput.size());
    const int writeErrno = GetErrno();
    if (bytesWritten > 0) {
      pendingOutput.erase(0, static_cast<size_t>(bytesWritten));
      continue;
    }
    if (bytesWritten < 0 &&
        (writeErrno == EAGAIN || writeErrno == EWOULDBLOCK ||
         writeErrno == ETIMEDOUT)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    if (bytesWritten == 0) {
      throw std::runtime_error("Socket closed during terminal output write");
    }
    throw std::runtime_error(string("Terminal output write failed: ") +
                             strerror(writeErrno));
  }
}
}  // namespace

UserTerminalHandler::UserTerminalHandler(
    shared_ptr<SocketHandler> _socketHandler, shared_ptr<UserTerminal> _term,
    bool _noratelimit, const optional<SocketEndpoint> routerEndpoint,
    const string& idPasskey)
    : routerFd(-1),
      socketHandler(_socketHandler),
      term(_term),
      noratelimit(_noratelimit),
      shuttingDown(false),
      pipeMode(false),
      routerEndpoint(routerEndpoint),
      ptyActive(false),
      hadReverseTunnels(false),
      disconnectTimeoutSeconds(std::nullopt) {
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
  tui.set_uid(0);
  tui.set_gid(0);
  tui.set_ptyactive(ptyActive);
  tui.set_hadreversetunnels(hadReverseTunnels);
  if (disconnectTimeoutSeconds) {
    tui.set_disconnect_timeout_seconds(*disconnectTimeoutSeconds);
  }

  routerFd = ServerFifoPath::detectAndConnect(routerEndpoint, socketHandler);
  socketHandler->writePacket(
      routerFd,
      Packet(TerminalPacketType::TERMINAL_USER_INFO, protoToString(tui)));
}

int UserTerminalHandler::reconnectRouter() {
  if (routerFd >= 0) {
    socketHandler->close(routerFd);
    routerFd = -1;
  }
  if (pipeMode) {
    // A restarted server can't tell that the stream is packet-framed.
    LOG(INFO) << "Router connection lost; ending pipe command session.";
    term->terminate();
    term->handleSessionEnd();
    lock_guard<recursive_mutex> guard(shutdownMutex);
    shuttingDown = true;
    return -1;
  }
  LOG(INFO) << "Router connection lost; the session stays alive and waits for "
               "the router to come back.";
  int backoffSec = 1;
  while (true) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        return -1;
      }
    }
    try {
      registerWithRouter();
      socketHandler->minimizeKernelBuffering(routerFd);
      LOG(INFO) << "Reconnected to the router; resuming the session.";
      return routerFd;
    } catch (const std::exception& re) {
      VLOG(1) << "Router not available yet: " << re.what();
      if (routerFd >= 0) {
        socketHandler->close(routerFd);
        routerFd = -1;
      }
    }
    for (int a = 0; a < backoffSec; a++) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        return -1;
      }
    }
    backoffSec = std::min(backoffSec * 2, 30);
  }
}

void UserTerminalHandler::forwardOutputToRouter(const char* data, size_t length,
                                                bool isStderr) {
  if (length == 0) {
    return;
  }
  string filtered;
  const char* outData = data;
  size_t outLength = length;
  // Pipe mode is a raw command channel. Stderr is not the tmux -CC stream.
  if (!pipeMode && !isStderr) {
    filtered = controlOutputFilter_.apply(string(data, length));
    if (filtered.empty()) {
      return;
    }
    outData = filtered.data();
    outLength = filtered.size();
  }
  TerminalBuffer tb;
  tb.set_buffer(string(outData, outLength));
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
    try {
      if (!socketHandler->readPacket(routerFd, &termInitPacket)) {
        continue;
      }
    } catch (const std::runtime_error& re) {
      LOG(INFO) << "Router disconnected before terminal initialization: "
                << re.what();
      if (reconnectRouter() < 0) {
        return;
      }
      continue;
    }
    if (termInitPacket.getHeader() != TerminalPacketType::TERMINAL_INIT) {
      STFATAL << "Invalid terminal init packet header: "
              << termInitPacket.getHeader();
    }
    TermInit ti = stringToProto<TermInit>(termInitPacket.getPayload());
    hadReverseTunnels = ti.hadreversetunnels();
    if (ti.has_disconnect_timeout_seconds()) {
      disconnectTimeoutSeconds = ti.disconnect_timeout_seconds();
    }
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
  ptyActive = true;
  runUserTerminal(masterfd);
  if (routerFd >= 0) {
    socketHandler->close(routerFd);
    routerFd = -1;
  }
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
  bool killRequested = false;
  // Held until the router consumes it, so a reconnect resends the suffix.
  string pendingOutput;
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
          if (reconnectRouter() < 0) {
            break;
          }
          continue;
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
              killRequested = handleTerminalInfo(*term, ti);
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
      if (killRequested) {
        lock_guard<recursive_mutex> guard(shutdownMutex);
        shuttingDown = true;
        break;
      }

      // ConPTY -> router output as TERMINAL_BUFFER packets (matches Unix).
      pendingOutput += conpty.drainOutput();
      writePendingOutput(socketHandler, routerFd, pendingOutput);

      if (!conpty.isRunning()) {
        pendingOutput += conpty.drainOutput();
        writePendingOutput(socketHandler, routerFd, pendingOutput);
        LOG(INFO) << "Terminal session ended";
        finishSession();
        lock_guard<recursive_mutex> guard(shutdownMutex);
        shuttingDown = true;
        break;
      }
    } catch (const std::exception& ex) {
      LOG(INFO) << ex.what();
      if (reconnectRouter() < 0) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  term->cleanup();
}

void UserTerminalHandler::runSocketTerminal(int masterFd) {
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  string pendingInput;
  // Do not read another terminal chunk until this one is delivered; this
  // preserves output across a failed write and bounds socket-backed buffering.
  string pendingOutput;
  const size_t maxPendingInput = 256 * 1024;
  const int inputFd = term->getInputFd();
  int activeStderrFd = term->getStderrFd();
  bool killRequested = false;

  while (true) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }
    const bool routerWritable = isSocketWritable(routerFd);
    const bool termReadable = pendingOutput.empty() && routerWritable &&
                              waitOnSocketData(masterFd, 0, 0);
    const bool stderrReadable = routerWritable && activeStderrFd >= 0 &&
                                waitOnSocketData(activeStderrFd, 0, 0);
    const bool routerReadable = pendingInput.length() < maxPendingInput &&
                                socketHandler->hasData(routerFd);
    if (!termReadable && !stderrReadable && !routerReadable &&
        pendingInput.empty() && pendingOutput.empty()) {
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
          if (pipeMode) {
            forwardOutputToRouter(b, static_cast<size_t>(rc), false);
          } else {
            pendingOutput.append(b, static_cast<size_t>(rc));
          }
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
          if (reconnectRouter() < 0) {
            break;
          }
          continue;
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
            killRequested = handleTerminalInfo(*term, ti);
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
      if (killRequested) {
        lock_guard<recursive_mutex> guard(shutdownMutex);
        shuttingDown = true;
        break;
      }

      if (!pendingOutput.empty()) {
        writePendingOutput(socketHandler, routerFd, pendingOutput);
        VLOG(4) << "Write to client";
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
      if (reconnectRouter() < 0) {
        break;
      }
    }
  }

  term->cleanup();
}
}  // namespace et
