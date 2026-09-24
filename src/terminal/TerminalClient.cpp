#include "TerminalClient.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "PseudoTerminalConsole.hpp"
#include "RawSocketUtils.hpp"
#include "SocksUtils.hpp"
#include "TelemetryService.hpp"
#include "TmuxCcFilter.hpp"
#include "TunnelUtils.hpp"
#include "WriteBuffer.hpp"

namespace et {
namespace {
// Printed by a commanded passenger after the isolated subshell returns so the
// ControlPersist bridge can recover a real exit status without killing the
// shared remote shell via `; exit`.
constexpr char kPassengerExitMarker[] = "__ET_PASSENGER_EXIT__:";

bool consumePassengerExitMarker(string* carry, const string& chunk,
                                string* forward,
                                optional<uint32_t>* parsedStatus) {
  carry->append(chunk);
  forward->clear();
  const size_t markerLen = sizeof(kPassengerExitMarker) - 1;
  while (true) {
    size_t pos = carry->find(kPassengerExitMarker);
    if (pos == string::npos) {
      size_t suffix = 0;
      for (size_t n = min(carry->size(), markerLen - 1); n > 0; --n) {
        if (carry->compare(carry->size() - n, n, kPassengerExitMarker, n) ==
            0) {
          suffix = n;
          break;
        }
      }
      if (carry->size() > suffix) {
        forward->append(carry->data(), carry->size() - suffix);
      }
      *carry = carry->substr(carry->size() - suffix);
      return false;
    }
    forward->append(carry->data(), pos);
    size_t statusStart = pos + markerLen;
    size_t end = carry->find('\n', statusStart);
    if (end == string::npos) {
      *carry = carry->substr(pos);
      return false;
    }
    string statusStr = carry->substr(statusStart, end - statusStart);
    char* endp = nullptr;
    unsigned long val = strtoul(statusStr.c_str(), &endp, 10);
    if (endp != statusStr.c_str()) {
      *parsedStatus = static_cast<uint32_t>(val);
    } else {
      *parsedStatus = 255;
    }
    *carry = carry->substr(end + 1);
    if (!carry->empty()) {
      forward->append(*carry);
      carry->clear();
    }
    return true;
  }
}
}  // namespace
std::atomic<bool> TerminalClient::closeOnHangup(false);
std::atomic<bool> TerminalClient::hangupCloseRequested(false);
std::atomic<bool> TerminalClient::hangupCloseCompleted(false);

const string TerminalClient::INVALID_SESSION_CONNECT_ERROR =
    "Server has no session for this client id";

TerminalClient::TerminalClient(
    shared_ptr<SocketHandler> _socketHandler,
    shared_ptr<SocketHandler> _pipeSocketHandler,
    const SocketEndpoint& _socketEndpoint, const string& id,
    const string& passkey, shared_ptr<Console> _console, bool jumphost,
    const string& tunnels, const string& reverseTunnels, bool forwardSshAgent,
    const string& identityAgent, int _keepaliveDuration,
    const vector<pair<string, string>>& envVars, bool _noPty,
    const string& command, const vector<string>& dynamicForwards,
    const string& stdioForward, int _maxConnectAttempts,
    bool _resumeSavedSession, std::function<bool()> _sessionHeartbeat,
    std::function<bool(const string&)> _sessionTitleUpdate)
    : console(_console),
      shuttingDown(false),
      keepaliveDuration(_keepaliveDuration),
      noPty(_noPty),
      stdioForwardActive(!stdioForward.empty()),
      sessionHeartbeat(_sessionHeartbeat),
      sessionTitleUpdate(_sessionTitleUpdate) {
  portForwardHandler = shared_ptr<PortForwardHandler>(
      new PortForwardHandler(_socketHandler, _pipeSocketHandler));
  InitialPayload payload;
  payload.set_jumphost(jumphost);
  payload.set_supports_exit_status(true);
  if (stdioForwardActive) {
    payload.set_no_shell(true);
  } else if (noPty) {
    payload.set_no_pty(true);
    payload.set_command(command);
  }

  for (const auto& envVar : envVars) {
    (*payload.mutable_environmentvariables())[envVar.first] = envVar.second;
  }

  try {
    if (tunnels.length()) {
      auto pfsrs = parseRangesToRequests(tunnels);
      for (auto& pfsr : pfsrs) {
        auto pfsresponse =
            portForwardHandler->createSource(pfsr, nullptr, -1, -1);
        if (pfsresponse.has_error()) {
          LOG(WARNING) << "Failed to establish port forward " << pfsr.source()
                       << " -> " << pfsr.destination() << " - "
                       << pfsresponse.error();
          continue;
        }
      }
    }
    for (const auto& dynamicArg : dynamicForwards) {
      SocketEndpoint socksSource = parseDynamicForwardArg(dynamicArg);
      auto response = portForwardHandler->createSocksSource(socksSource);
      if (response.has_error()) {
        LOG(WARNING) << "Failed to establish dynamic forward " << dynamicArg
                     << " - " << response.error();
        continue;
      }
    }
    if (stdioForwardActive) {
#ifdef WIN32
      // CRT fds 0/1 are not sockets. The Windows poller ignores fd 0 and
      // SocketHandler uses recv/send, so -W cannot bridge stdio yet.
      CLOG(INFO, "stdout") << "-W/--stdio-forward is not supported on Windows"
                           << endl;
      exit(1);
#else
      SocketEndpoint destination = parseStdioForwardArg(stdioForward);
      auto response = portForwardHandler->createStdioForward(
          destination, STDIN_FILENO, STDOUT_FILENO, false);
      if (response.has_error()) {
        CLOG(INFO, "stdout")
            << "Error establishing stdio forward: " << response.error() << endl;
        exit(1);
      }
#endif
    }
    if (reverseTunnels.length()) {
      auto pfsrs = parseRangesToRequests(reverseTunnels);
      for (auto& pfsr : pfsrs) {
        *(payload.add_reversetunnels()) = pfsr;
      }
    }
    if (forwardSshAgent) {
      PortForwardSourceRequest pfsr;
      string authSock = "";
      if (identityAgent.length()) {
        authSock.assign(identityAgent);
      } else {
        auto authSockEnv = getenv("SSH_AUTH_SOCK");
        if (!authSockEnv) {
          CLOG(INFO, "stdout")
              << "Missing environment variable SSH_AUTH_SOCK.  Are you sure "
                 "you "
                 "ran ssh-agent first?"
              << endl;
          exit(1);
        }
        authSock.assign(authSockEnv);
      }
      if (authSock.length()) {
        pfsr.mutable_destination()->set_name(authSock);
        pfsr.set_environmentvariable("SSH_AUTH_SOCK");
        *(payload.add_reversetunnels()) = pfsr;
      }
    }
  } catch (const std::runtime_error& ex) {
    CLOG(INFO, "stdout") << "Error establishing port forward: " << ex.what()
                         << endl;
    exit(1);
  }

  connection = shared_ptr<ClientConnection>(
      new ClientConnection(_socketHandler, _socketEndpoint, id, passkey,
                           /*_resetIntent=*/_resumeSavedSession));

  int connectFailCount = 0;
  while (true) {
    try {
      bool fail = true;
      if (connection->connect()) {
        if (connection->wasRecovered()) {
          // Reattached: the session was bootstrapped when it started.
          fail = false;
        } else {
          connection->writePacket(
              Packet(EtPacketType::INITIAL_PAYLOAD, protoToString(payload)));
          for (int a = 0; a < 3; a++) {
            int clientFd = connection->getSocketFd();
            if (clientFd < 0) {
              std::this_thread::sleep_for(std::chrono::seconds(1));
              continue;
            }
            if (waitOnSocketData(clientFd)) {
              Packet initialResponsePacket;
              if (connection->readPacket(&initialResponsePacket)) {
                if (initialResponsePacket.getHeader() !=
                    EtPacketType::INITIAL_RESPONSE) {
                  CLOG(INFO, "stdout") << "Error: Missing initial response\n";
                  STFATAL << "Missing initial response!";
                }
                auto initialResponse = stringToProto<InitialResponse>(
                    initialResponsePacket.getPayload());
                if (initialResponse.has_error()) {
                  CLOG(INFO, "stdout") << "Error initializing connection: "
                                       << initialResponse.error() << endl;
                  exit(1);
                }
                fail = false;
                break;
              }
            }
          }
        }
      }
      if (fail) {
        LOG(WARNING) << "Connecting to server failed: Connect timeout";
        connectFailCount++;
        if (_resumeSavedSession && connection &&
            connection->lastStatus() == et::ConnectStatus::INVALID_KEY) {
          throw std::runtime_error(INVALID_SESSION_CONNECT_ERROR);
        }
        if (connectFailCount >= _maxConnectAttempts) {
          throw std::runtime_error("Connect Timeout");
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
    } catch (const runtime_error& err) {
      LOG(INFO) << "Could not make initial connection to server";
      if (_resumeSavedSession && connection &&
          connection->lastStatus() == et::ConnectStatus::INVALID_KEY) {
        connection->shutdown();
        throw std::runtime_error(INVALID_SESSION_CONNECT_ERROR);
      }
      if (!_resumeSavedSession) {
        CLOG(INFO, "stdout") << "Could not make initial connection to "
                             << _socketEndpoint << ": " << err.what() << endl;
        exit(1);
      }
      throw;
    }

    TelemetryService::get()->logToDatadog("Connection Established",
                                          el::Level::Info, __FILE__, __LINE__);
    break;
  }
  // The client id is half of the reconnect credential.
  VLOG(1) << "Client created";
};

TerminalClient::~TerminalClient() {
  connection->shutdown();
  console.reset();
  portForwardHandler.reset();
  connection.reset();
}

bool TerminalClient::killSession(int timeoutSeconds) {
  TerminalInfo command;
  command.set_command(TerminalInfo::KILL_SESSION);
  command.set_commandversion(SESSION_KILL_COMMAND_VERSION);
  connection->writePacket(
      Packet(TerminalPacketType::TERMINAL_INFO, protoToString(command)));

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (connection->hasData()) {
      Packet packet;
      if (connection->read(&packet) &&
          packet.getHeader() == TerminalPacketType::KEEP_ALIVE &&
          packet.getPayload() == SESSION_KILL_ACK) {
        return true;
      }
    }
    if (connection->lastStatus() == ConnectStatus::INVALID_KEY) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

int TerminalClient::run(const string& command, const bool noexit) {
  if (console) {
    console->setup();
  }

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  time_t keepaliveTime = time(NULL) + keepaliveDuration;
  bool waitingOnKeepalive = false;
  const bool wantRemoteExitStatus = !command.empty() && !noexit;
  int remoteExitStatus = 0;
  bool haveRemoteExitStatus = false;
  time_t sessionHeartbeatTime = time(NULL);
  bool sessionHeartbeatWarningLogged = false;
  time_t sessionTitleUpdateTime = time(NULL);
  bool sessionTitleWarningLogged = false;
  optional<string> currentSessionTitle;
  optional<string> pendingSessionTitle;

  if (command.length() && !noPty) {
    LOG(INFO) << "Got command: " << command;
    et::TerminalBuffer tb;
    if (noexit)
      tb.set_buffer(command + "\n");
    else
      tb.set_buffer(command + "; exit\n");

    connection->writePacket(
        Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
  } else if (command.length() && noPty) {
    LOG(INFO) << "Raw pipe command (no shell injection): " << command;
  }

  TerminalInfo lastTerminalInfo;

  if (!console.get() && !stdioForwardActive) {
    // NOTE: ../../scripts/ssh-et relies on the wording of this message, so if
    // you change it please update it as well.
    CLOG(INFO, "stdout") << "ET running, feel free to background..." << endl;
  }

  // Launchers such as nohup(1) replace stdout with a regular file while
  // leaving the tty on stdin, so the descriptor Console exposes for input is
  // readable-but-empty (or not readable at all). select() always reports it
  // ready and every read yields EOF/EBADF, which must not be mistaken for the
  // user closing an interactive session -- doing so would tear down a
  // backgrounded client and its port forwards immediately.
  bool consoleInputDisabled = false;
  string consoleInterruptCarry;
  WriteBuffer consoleOut;
  while (!connection->isShuttingDown()) {
    if (closeOnHangup && hangupCloseRequested) {
      try {
        if (!connection->isDisconnected()) {
          connection->writePacket(
              Packet(TerminalPacketType::TERMINAL_CLOSE, ""));
        }
      } catch (...) {
      }
      hangupCloseCompleted = true;
      break;
    }
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }
    int consoleFd = -1;
    if (console && !consoleInputDisabled) {
      consoleFd = console->getFd();
    }
    const bool consoleWritable = console && consoleOut.hasPendingData();
    const int clientFd = connection->getSocketFd();
    const bool watchClient =
        clientFd > 0 && consoleOut.size() < WriteBuffer::FLUSH_THRESHOLD;
    // Include port forward sockets for low-latency forwarding.
    set<int> pfFds;
    portForwardHandler->getForwardFds(&pfFds);

    set<int> readyFds;
#ifdef WIN32
    vector<WSAPOLLFD> pollFds;
    auto watch = [&pollFds](int fd, short events) {
      if (fd <= 0) return;
      for (auto& pollFd : pollFds) {
        if (pollFd.fd == static_cast<SOCKET>(fd)) {
          pollFd.events |= events;
          return;
        }
      }
      WSAPOLLFD pfd = {};
      pfd.fd = static_cast<SOCKET>(fd);
      pfd.events = events;
      pollFds.push_back(pfd);
    };
    auto* pseudoConsole = dynamic_cast<PseudoTerminalConsole*>(console.get());
    if (consoleFd >= 0 && !pseudoConsole) {
      watch(consoleFd, POLLRDNORM);
    }
    if (consoleWritable && !pseudoConsole) {
      watch(console->getFd(), POLLWRNORM);
    }
    if (watchClient) {
      watch(clientFd, POLLRDNORM);
    }
    for (int fd : pfFds) {
      watch(fd, POLLRDNORM);
    }
    if (!pollFds.empty()) {
      const int pollResult =
          ::WSAPoll(pollFds.data(), static_cast<ULONG>(pollFds.size()), 10);
      if (pollResult > 0) {
        for (const auto& pollFd : pollFds) {
          if ((pollFd.events & (POLLRDNORM | POLLRDBAND)) != 0 &&
              (pollFd.revents &
               (POLLRDNORM | POLLRDBAND | POLLERR | POLLHUP | POLLNVAL)) != 0) {
            readyFds.insert(static_cast<int>(pollFd.fd));
          }
        }
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#else
    // poll() has no FD_SETSIZE ceiling, and unlike epoll it accepts the
    // regular file nohup(1) leaves on the console descriptor.
    vector<struct pollfd> pollFds;
    // The console read and write descriptors are the same fd, so interest is
    // merged rather than appended.
    auto watch = [&pollFds](int fd, short events) {
      for (auto& pollFd : pollFds) {
        if (pollFd.fd == fd) {
          pollFd.events |= events;
          return;
        }
      }
      pollFds.push_back({fd, events, 0});
    };
    if (consoleFd >= 0) {
      watch(consoleFd, POLLIN);
      // PseudoTerminalConsole writes to stdout and reads keystrokes from
      // stdin. FakeConsole (tests) uses one pipe for both; watching the
      // process stdin there races an always-ready EOF and disables input.
      if (consoleFd == STDOUT_FILENO) {
        watch(STDIN_FILENO, POLLIN);
      }
    }
    if (consoleWritable) {
      // Only needs to wake the loop; the drain below re-checks writability.
      watch(console->getFd(), POLLOUT);
    }
    if (watchClient) {
      watch(clientFd, POLLIN);
    }
    for (int fd : pfFds) {
      watch(fd, POLLIN);
    }
    const int pollResult =
        poll(pollFds.data(), static_cast<nfds_t>(pollFds.size()), 10);
    if (pollResult < 0 && errno != EINTR) {
      FATAL_FAIL(pollResult);
    }
    for (const auto& pollFd : pollFds) {
      if ((pollFd.events & POLLIN) != 0 &&
          (pollFd.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
        readyFds.insert(pollFd.fd);
      }
    }
#endif

    try {
      bool skipServerRead = false;
      if (console && consoleFd >= 0) {
        bool inputReady = readyFds.count(consoleFd) != 0;
#ifndef WIN32
        if (consoleFd == STDOUT_FILENO) {
          inputReady = inputReady || readyFds.count(STDIN_FILENO) != 0;
        }
#endif
        if (inputReady) {
          // Read from stdin and write to our client that will then send it to
          // the server.
          VLOG(4) << "Got data from stdin";
#ifdef WIN32
          auto* pseudoConsole =
              dynamic_cast<PseudoTerminalConsole*>(console.get());
          if (pseudoConsole) {
            HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
            DWORD consoleMode = 0;
            if (handle == NULL || handle == INVALID_HANDLE_VALUE ||
                !GetConsoleMode(handle, &consoleMode)) {
              // stdin is redirected (nohup/background/service): there is no
              // keyboard to read, but the session must survive. Mirrors the
              // Unix non-tty path below.
              LOG(INFO) << "Console stdin is not a console, disabling "
                           "console input";
              consoleInputDisabled = true;
            } else {
              DWORD events = 0;
              INPUT_RECORD buffer[128];
              if (!PeekConsoleInput(handle, buffer, 128, &events)) {
                events = 0;
              }
              if (events > 0) {
                if (!ReadConsoleInput(handle, buffer, 128, &events)) {
                  events = 0;
                }
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
                  et::TerminalBuffer tb;
                  tb.set_buffer(s);

                  connection->writePacket(Packet(
                      TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
                  keepaliveTime = time(NULL) + keepaliveDuration;
                  if (WriteBuffer::containsInterruptByte(s) ||
                      tmuxCcInputRequestsInterrupt(consoleInterruptCarry, s)) {
                    skipServerRead = true;
                    consoleOut.filterDroppable();
                    LOG(INFO) << "Interrupt from stdin (" << s.size()
                              << " bytes), consoleOut=" << consoleOut.size();
                  }
                  tmuxCcRetainIncompleteLine(&consoleInterruptCarry, s);
                }
              }
            }
          } else {
            // Socket-backed console (e.g. FakeConsole in integration tests).
            char b[BUF_SIZE];
            int rc = ::recv(consoleFd, b, BUF_SIZE, 0);
            int savedErrno = GetErrno();
            if (rc > 0) {
              string s(b, rc);
              et::TerminalBuffer tb;
              tb.set_buffer(s);

              connection->writePacket(Packet(
                  TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
              keepaliveTime = time(NULL) + keepaliveDuration;
              if (WriteBuffer::containsInterruptByte(s) ||
                  tmuxCcInputRequestsInterrupt(consoleInterruptCarry, s)) {
                skipServerRead = true;
                consoleOut.filterDroppable();
                LOG(INFO) << "Interrupt from stdin (" << s.size()
                          << " bytes), consoleOut=" << consoleOut.size();
              }
              tmuxCcRetainIncompleteLine(&consoleInterruptCarry, s);
            } else if (rc == 0) {
              LOG(INFO) << "Console is at EOF, disabling console input";
              consoleInputDisabled = true;
            } else {
              if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
                // Transient error, retry
              } else {
                LOG(INFO) << "Console read error (" << savedErrno
                          << "): " << strerror(savedErrno)
                          << ", disabling console input";
                consoleInputDisabled = true;
              }
            }
          }
#else
          if (console) {
            int readFd = consoleFd;
            if (consoleFd == STDOUT_FILENO &&
                readyFds.count(STDIN_FILENO) != 0) {
              readFd = STDIN_FILENO;
            }
            int rc = ::read(readFd, b, BUF_SIZE);
            int savedErrno = errno;  // Save errno before any logging
            if (rc > 0) {
              // VLOG(1) << "Sending byte: " << int(b) << " " << char(b) << " "
              // << connection->getWriter()->getSequenceNumber();
              string s(b, rc);
              et::TerminalBuffer tb;
              tb.set_buffer(s);

              connection->writePacket(Packet(
                  TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
              keepaliveTime = time(NULL) + keepaliveDuration;
              if (WriteBuffer::containsInterruptByte(s) ||
                  tmuxCcInputRequestsInterrupt(consoleInterruptCarry, s)) {
                skipServerRead = true;
                consoleOut.filterDroppable();
                LOG(INFO) << "Interrupt from stdin (" << s.size()
                          << " bytes), consoleOut=" << consoleOut.size();
              }
              tmuxCcRetainIncompleteLine(&consoleInterruptCarry, s);
            } else if (rc == 0) {
              if (isatty(consoleFd)) {
                LOG(INFO) << "Console EOF";
                break;
              }
              LOG(INFO) << "Console is not a tty and is at EOF, disabling "
                           "console input";
              consoleInputDisabled = true;
            } else {
              if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
                // Transient error, retry
              } else if (!isatty(consoleFd)) {
                LOG(INFO) << "Console is not a tty and cannot be read ("
                          << savedErrno << "): " << strerror(savedErrno)
                          << ", disabling console input";
                consoleInputDisabled = true;
              } else {
                LOG(INFO) << "Console read error: (" << savedErrno
                          << "): " << strerror(savedErrno);
                break;
              }
            }
          }
#endif
        }
      }

      if (!skipServerRead && clientFd > 0 && readyFds.count(clientFd) != 0) {
        VLOG(4) << "Clientfd is selected";
        // Cap how much we pull from the server so Ctrl+C can be handled
        // before megabytes of flood are painted. Sequence numbers are
        // assigned on the server at writePacket, so unread socket data
        // stays in order.
        while (connection->hasData() &&
               consoleOut.size() < WriteBuffer::FLUSH_THRESHOLD) {
          VLOG(4) << "connection has data";
          Packet packet;
          if (!connection->read(&packet)) {
            break;
          }
          uint8_t packetType = packet.getHeader();
          if (packetType == et::TerminalPacketType::PORT_FORWARD_DATA ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE) {
            keepaliveTime = time(NULL) + keepaliveDuration;
            VLOG(4) << "Got PF packet type " << packetType;
            portForwardHandler->handlePacket(packet, connection);
            continue;
          }
          switch (packetType) {
            case et::TerminalPacketType::TERMINAL_BUFFER: {
              VLOG(3) << "Got terminal buffer";
              et::TerminalBuffer tb =
                  stringToProto<et::TerminalBuffer>(packet.getPayload());
              keepaliveTime = time(NULL) + keepaliveDuration;
              if (tb.is_stderr()) {
#ifdef WIN32
                auto hstderr = GetStdHandle(STD_ERROR_HANDLE);
                DWORD written = 0;
                WriteFile(hstderr, tb.buffer().data(),
                          static_cast<DWORD>(tb.buffer().size()), &written,
                          NULL);
#else
                RawSocketUtils::writeAll(STDERR_FILENO, tb.buffer().data(),
                                         tb.buffer().size());
#endif
              } else if (console) {
                if (sessionTitleUpdate && !tb.buffer().empty()) {
                  const optional<string> parsedTitle =
                      titleParser.parse(tb.buffer());
                  if (parsedTitle && (!currentSessionTitle ||
                                      *parsedTitle != *currentSessionTitle)) {
                    currentSessionTitle = parsedTitle;
                    pendingSessionTitle = parsedTitle;
                  }
                }
                consoleOut.enqueue(tb.buffer());
              } else {
#ifdef WIN32
                auto hstdout = GetStdHandle(STD_OUTPUT_HANDLE);
                DWORD written = 0;
                WriteFile(hstdout, tb.buffer().data(),
                          static_cast<DWORD>(tb.buffer().size()), &written,
                          NULL);
#else
                RawSocketUtils::writeAll(STDOUT_FILENO, tb.buffer().data(),
                                         tb.buffer().size());
#endif
              }
              break;
            }
            case et::TerminalPacketType::KEEP_ALIVE:
              waitingOnKeepalive = false;
              // This will fill up log file quickly but is helpful for debugging
              // latency issues.
              LOG(INFO) << "Got a keepalive";
              break;
            case et::TerminalPacketType::TERMINAL_EXIT_STATUS: {
              et::TerminalExitStatus tes =
                  stringToProto<et::TerminalExitStatus>(packet.getPayload());
              if (tes.has_exitcode()) {
                remoteExitStatus = tes.exitcode();
                haveRemoteExitStatus = true;
                LOG(INFO) << "Got remote exit status " << remoteExitStatus;
              }
              // Do not set shuttingDown yet: writeSome may return partial/zero
              // bytes (BinaryStdioConsole / O_NONBLOCK). Stop only after
              // consoleOut has drained.
              break;
            }
            default:
              STFATAL << "Unknown packet type: " << int(packetType);
          }
        }
      }

      if (console && consoleOut.hasPendingData()) {
        size_t count = 0;
        const char* data = consoleOut.peekData(&count);
        if (data != nullptr && count > 0) {
          size_t written = console->writeSome(string(data, count));
          if (written > 0) {
            consoleOut.consume(written);
          }
        }
      }

      // Command sessions: stop only once remote status is known and any
      // buffered console output has been written (or there is no console).
      if (wantRemoteExitStatus && haveRemoteExitStatus &&
          (!console || !consoleOut.hasPendingData())) {
        lock_guard<recursive_mutex> guard(shutdownMutex);
        shuttingDown = true;
      }

      if (clientFd > 0 && keepaliveTime < time(NULL)) {
        keepaliveTime = time(NULL) + keepaliveDuration;
        if (waitingOnKeepalive) {
          LOG(INFO) << "Missed a keepalive, killing connection.";
          connection->closeSocketAndMaybeReconnect();
          waitingOnKeepalive = false;
        } else {
          LOG(INFO) << "Writing keepalive packet";
          connection->writePacket(Packet(TerminalPacketType::KEEP_ALIVE, ""));
          waitingOnKeepalive = true;
        }
      }
      if (clientFd < 0) {
        // We are disconnected, so stop waiting for keepalive.
        waitingOnKeepalive = false;
      }

      if (console) {
        auto ti = console->getTerminalInfo();

        if (ti && *ti != lastTerminalInfo) {
          VLOG(1) << "Window size changed: row: " << ti->row()
                  << " column: " << ti->column() << " width: " << ti->width()
                  << " height: " << ti->height();
          lastTerminalInfo = *ti;
          connection->writePacket(
              Packet(TerminalPacketType::TERMINAL_INFO, protoToString(*ti)));
        }
      }

      vector<PortForwardDestinationRequest> requests;
      vector<PortForwardData> dataToSend;
#ifdef WIN32
      // select() silently drops descriptors past FD_SETSIZE, so readiness is
      // not authoritative here and every handler has to be checked.
      portForwardHandler->update(&requests, &dataToSend);
#else
      portForwardHandler->update(&requests, &dataToSend, &readyFds);
#endif
      for (auto& pfr : requests) {
        connection->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST,
                   protoToString(pfr)));
        VLOG(4) << "send PF request";
        keepaliveTime = time(NULL) + keepaliveDuration;
      }
      for (auto& pwd : dataToSend) {
        connection->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DATA, protoToString(pwd)));
        VLOG(4) << "send PF data";
        keepaliveTime = time(NULL) + keepaliveDuration;
      }
      if (stdioForwardActive && !portForwardHandler->hasActiveStdioForward()) {
        connection->writePacket(Packet(TerminalPacketType::TERMINAL_CLOSE, ""));
        lock_guard<recursive_mutex> guard(shutdownMutex);
        shuttingDown = true;
      }

      const time_t now = time(NULL);
      if (sessionHeartbeat && connection->getSocketFd() > 0 &&
          sessionHeartbeatTime <= now) {
        bool heartbeatSucceeded = false;
        try {
          heartbeatSucceeded = sessionHeartbeat();
        } catch (...) {
          // Best-effort: never end the connection over session storage.
        }
        if (!heartbeatSucceeded && !sessionHeartbeatWarningLogged) {
          LOG(WARNING) << "Could not update saved session heartbeat";
          sessionHeartbeatWarningLogged = true;
        }
        sessionHeartbeatTime = now + 15;
      }
      if (sessionTitleUpdate && pendingSessionTitle &&
          connection->getSocketFd() > 0 && sessionTitleUpdateTime <= now) {
        bool updateSucceeded = false;
        try {
          updateSucceeded = sessionTitleUpdate(*pendingSessionTitle);
        } catch (...) {
        }
        if (updateSucceeded) {
          pendingSessionTitle.reset();
        } else if (!sessionTitleWarningLogged) {
          LOG(WARNING) << "Could not update saved session title";
          sessionTitleWarningLogged = true;
        }
        sessionTitleUpdateTime = now + 2;
      }
    } catch (const runtime_error& re) {
      STERROR << "Error: " << re.what();
      CLOG(INFO, "stdout") << "Connection closing because of error: "
                           << re.what() << endl;
      lock_guard<recursive_mutex> guard(shutdownMutex);
      shuttingDown = true;
    }
  }
  // Finish writing buffered remote output before tearing down the console.
  // EXIT_STATUS or a dying connection must not discard consoleOut: writeSome
  // may return 0 under O_NONBLOCK (BinaryStdioConsole) until the fd is
  // writable. Hard errors (EPIPE/EBADF) stop the drain but must not throw past
  // run() when remoteExitStatus is already known.
  if (console) {
    while (consoleOut.hasPendingData()) {
      size_t count = 0;
      const char* data = consoleOut.peekData(&count);
      if (data == nullptr || count == 0) {
        break;
      }
      try {
        size_t written = console->writeSome(string(data, count));
        if (written > 0) {
          consoleOut.consume(written);
          continue;
        }
      } catch (const runtime_error& re) {
        STERROR << "Error draining consoleOut: " << re.what();
        break;
      }
#ifndef WIN32
      pollfd pfd = {console->getFd(), POLLOUT, 0};
      if (poll(&pfd, 1, 10) < 0 && errno != EINTR) {
        break;
      }
#else
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
    }
    console->teardown();
  }
  if (!stdioForwardActive) {
    CLOG(INFO, "stdout") << "Session terminated" << endl;
  }
  if (wantRemoteExitStatus && haveRemoteExitStatus) {
    return remoteExitStatus;
  }
  return 0;
}

uint32_t TerminalClient::runPassengerSession(int inFd, int outFd, int errFd,
                                             const string& command) {
  if (!idleServicing.load()) {
    throw runtime_error(
        "mux session attach is only available while ControlPersist is "
        "servicing the transport");
  }
  {
    lock_guard<mutex> guard(passengerMutex);
    if (passenger.active) {
      throw runtime_error("another mux passenger session is already attached");
    }
    passenger.inFd = inFd;
    passenger.outFd = outFd;
    passenger.errFd = errFd;
    passenger.command = command;
    passenger.generation++;
    passenger.active = true;
    // Honor a hangup/cancel that arrived before we became active.
    if (passenger.cancelRequested) {
      passenger.exitStatus = 1;
      passenger.cancelRequested = false;
    } else {
      passenger.exitStatus.reset();
    }
  }
  unique_lock<mutex> lock(passengerMutex);
  passengerCv.wait(lock, [this]() { return passenger.exitStatus.has_value(); });
  uint32_t status = *passenger.exitStatus;
  passenger.active = false;
  passenger.cancelRequested = false;
  // Prefer suppress over clearing sticky so a late MuxMaster cancel that
  // races the return path cannot re-arm cancelRequested for the next attach.
  passenger.suppressStickyCancel = true;
  passenger.inFd = passenger.outFd = passenger.errFd = -1;
  // The idle loop may already have copied these fds and be inside poll/read/
  // write. Wait until that section finishes before the caller closes them.
  passengerCv.wait(lock, [this]() { return passenger.ioDepth == 0; });
  return status;
}

void TerminalClient::beginPassengerWatch() {
  lock_guard<mutex> guard(passengerMutex);
  // New mux watch: allow hangup-before-active sticky cancel again.
  passenger.suppressStickyCancel = false;
}

void TerminalClient::cancelPassengerSession() {
  lock_guard<mutex> guard(passengerMutex);
  if (passenger.active) {
    // Current attach is in flight or completing: finish it, but do not leave
    // sticky cancel for a later session (MuxMaster may re-enter this handler).
    if (!passenger.exitStatus.has_value()) {
      passenger.exitStatus = 1;
      passengerCv.notify_all();
    }
    // A late cancel after this attach returns must not sticky-arm the next one.
    passenger.suppressStickyCancel = true;
    return;
  }
  if (passenger.suppressStickyCancel) {
    return;
  }
  // Hangup before active: sticky until runPassengerSession attaches.
  passenger.cancelRequested = true;
}

void TerminalClient::serviceIdleUntil(const function<bool()>& keepGoing) {
  idleServicing = true;
  time_t keepaliveTime = time(NULL) + keepaliveDuration;
  bool waitingOnKeepalive = false;
  WriteBuffer passengerOut;
  // Like primary run()'s consoleInputDisabled: local stdin EOF must not be
  // treated as a successful remote session end (e.g. `et -S … -- cmd
  // </dev/null`).
  bool passengerInputDisabled = false;
  bool awaitingPassengerExitMarker = false;
  string passengerExitCarry;
  // Tracks which attach `serviceIdleUntil` has armed. Must not rely solely on
  // observing `!passenger.active`: a second attach can flip `active` again
  // before the idle loop sees the inactive gap, which would leave
  // passengerInputDisabled stuck and skip injecting the new command.
  uint64_t boundPassengerGeneration = 0;

  while (keepGoing() && !connection->isShuttingDown()) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }

    int passengerInFd = -1;
    int passengerOutFd = -1;
    struct IoHold {
      TerminalClient* self = nullptr;
      ~IoHold() {
        if (self == nullptr) {
          return;
        }
        lock_guard<mutex> guard(self->passengerMutex);
        if (self->passenger.ioDepth > 0) {
          self->passenger.ioDepth--;
        }
        self->passengerCv.notify_all();
      }
    } ioHold;
    {
      lock_guard<mutex> guard(passengerMutex);
      if (passenger.active && !passenger.exitStatus.has_value()) {
        if (boundPassengerGeneration != passenger.generation) {
          boundPassengerGeneration = passenger.generation;
          passengerInputDisabled = false;
          awaitingPassengerExitMarker = false;
          passengerExitCarry.clear();
        }
        if (!passengerInputDisabled) {
          passengerInFd = passenger.inFd;
        }
        passengerOutFd = passenger.outFd;
        if (passengerInFd >= 0 || passengerOutFd >= 0) {
          passenger.ioDepth++;
          ioHold.self = this;
        }
        // Key inject off non-empty command (cleared after write); do not keep
        // a sticky injectedPassengerCommand across missed inactive gaps.
        if (!passenger.command.empty()) {
          et::TerminalBuffer tb;
          if (noPty) {
            tb.set_buffer(passenger.command);
          } else {
            // Isolate in a subshell so an `exit` inside the passenger command
            // cannot kill the shared ControlPersist PTY, then print a marker
            // with $? so the bridge can propagate a real status.
            tb.set_buffer("(" + passenger.command + "); printf '\\n" +
                          string(kPassengerExitMarker) + "%d\\n' $?\n");
            awaitingPassengerExitMarker = true;
            passengerExitCarry.clear();
          }
          passenger.command.clear();
          try {
            connection->writePacket(
                Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
          } catch (...) {
          }
        }
      } else if (!passenger.active) {
        passengerInputDisabled = false;
        awaitingPassengerExitMarker = false;
        passengerExitCarry.clear();
      }
    }

    const int clientFd = connection->getSocketFd();
    set<int> pfFds;
    portForwardHandler->getForwardFds(&pfFds);
    set<int> readyFds;

#ifndef WIN32
    vector<struct pollfd> pollFds;
    auto watch = [&pollFds](int fd, short events) {
      if (fd < 0) {
        return;
      }
      for (auto& pollFd : pollFds) {
        if (pollFd.fd == fd) {
          pollFd.events |= events;
          return;
        }
      }
      pollFds.push_back({fd, events, 0});
    };
    if (passengerInFd >= 0) {
      watch(passengerInFd, POLLIN);
    }
    if (passengerOutFd >= 0 && passengerOut.hasPendingData()) {
      watch(passengerOutFd, POLLOUT);
    }
    if (clientFd > 0) {
      watch(clientFd, POLLIN);
    }
    for (int fd : pfFds) {
      watch(fd, POLLIN);
    }
    if (!pollFds.empty()) {
      int rc = ::poll(pollFds.data(), static_cast<nfds_t>(pollFds.size()), 50);
      if (rc > 0) {
        for (const auto& pollFd : pollFds) {
          if (pollFd.revents != 0) {
            readyFds.insert(pollFd.fd);
          }
        }
      }
    } else {
      this_thread::sleep_for(chrono::milliseconds(50));
    }
#else
    this_thread::sleep_for(chrono::milliseconds(50));
    (void)passengerInFd;
    (void)passengerOutFd;
#endif

    try {
#ifndef WIN32
      if (passengerInFd >= 0 && readyFds.count(passengerInFd)) {
        char b[4096];
        ssize_t rc = ::read(passengerInFd, b, sizeof(b));
        if (rc > 0) {
          et::TerminalBuffer tb;
          tb.set_buffer(string(b, rc));
          connection->writePacket(
              Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
          keepaliveTime = time(NULL) + keepaliveDuration;
        } else if (rc == 0 || (rc < 0 && errno != EAGAIN &&
                               errno != EWOULDBLOCK && errno != EINTR)) {
          // Mirror primary run(): disable further local input; keep bridging
          // until a real session-end signal (exit marker / TERMINAL_CLOSE /
          // idle teardown).
          passengerInputDisabled = true;
        }
      }
#endif

      if (clientFd > 0) {
        bool haveData = true;
#ifndef WIN32
        haveData = readyFds.count(clientFd) != 0 || connection->hasData();
#endif
        while (haveData && connection->hasData()) {
          Packet packet;
          if (!connection->readPacket(&packet)) {
            break;
          }
          auto packetType = packet.getHeader();
          switch (TerminalPacketType(packetType)) {
            case TerminalPacketType::TERMINAL_BUFFER: {
              et::TerminalBuffer tb =
                  stringToProto<et::TerminalBuffer>(packet.getPayload());
#ifndef WIN32
              if (passengerOutFd >= 0 || awaitingPassengerExitMarker) {
                if (awaitingPassengerExitMarker) {
                  string forward;
                  optional<uint32_t> parsedStatus;
                  bool complete = consumePassengerExitMarker(
                      &passengerExitCarry, tb.buffer(), &forward,
                      &parsedStatus);
                  if (!forward.empty() && passengerOutFd >= 0) {
                    passengerOut.enqueue(forward);
                  }
                  if (complete && parsedStatus.has_value()) {
                    lock_guard<mutex> guard(passengerMutex);
                    if (passenger.active && !passenger.exitStatus.has_value()) {
                      passenger.exitStatus = *parsedStatus;
                      passengerCv.notify_all();
                    }
                    awaitingPassengerExitMarker = false;
                    passengerExitCarry.clear();
                  }
                } else if (passengerOutFd >= 0) {
                  passengerOut.enqueue(tb.buffer());
                }
              }
#else
              (void)tb;
#endif
              break;
            }
            case TerminalPacketType::PORT_FORWARD_DATA:
            case TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST:
            case TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE:
              portForwardHandler->handlePacket(packet, connection);
              break;
            case TerminalPacketType::KEEP_ALIVE:
              waitingOnKeepalive = false;
              break;
            case TerminalPacketType::TERMINAL_CLOSE: {
              lock_guard<mutex> guard(passengerMutex);
              if (passenger.active && !passenger.exitStatus.has_value()) {
                // Interactive attach: clean remote close. Commanded attach
                // that never saw a marker: fail closed (status unknown).
                passenger.exitStatus = awaitingPassengerExitMarker ? 255u : 0u;
                passengerCv.notify_all();
              }
              awaitingPassengerExitMarker = false;
              passengerExitCarry.clear();
              break;
            }
            default:
              break;
          }
          haveData = connection->hasData();
        }
      }

#ifndef WIN32
      if (passengerOutFd >= 0 && passengerOut.hasPendingData()) {
        size_t count = 0;
        const char* data = passengerOut.peekData(&count);
        if (data != nullptr && count > 0) {
          ssize_t written = ::write(passengerOutFd, data, count);
          if (written > 0) {
            passengerOut.consume(static_cast<size_t>(written));
          }
        }
      }
#endif

      if (clientFd > 0 && keepaliveTime < time(NULL)) {
        keepaliveTime = time(NULL) + keepaliveDuration;
        if (waitingOnKeepalive) {
          connection->closeSocketAndMaybeReconnect();
          waitingOnKeepalive = false;
        } else {
          connection->writePacket(Packet(TerminalPacketType::KEEP_ALIVE, ""));
          waitingOnKeepalive = true;
        }
      }

      vector<PortForwardDestinationRequest> requests;
      vector<PortForwardData> dataToSend;
#ifndef WIN32
      portForwardHandler->update(&requests, &dataToSend, &readyFds);
#else
      portForwardHandler->update(&requests, &dataToSend);
#endif
      for (auto& pfr : requests) {
        connection->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST,
                   protoToString(pfr)));
        keepaliveTime = time(NULL) + keepaliveDuration;
      }
      for (auto& pwd : dataToSend) {
        connection->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DATA, protoToString(pwd)));
        keepaliveTime = time(NULL) + keepaliveDuration;
      }
    } catch (const runtime_error& re) {
      LOG(WARNING) << "ControlPersist service error: " << re.what();
      lock_guard<mutex> guard(passengerMutex);
      if (passenger.active && !passenger.exitStatus.has_value()) {
        passenger.exitStatus = 1;
        passengerCv.notify_all();
      }
      break;
    }
  }

  lock_guard<mutex> guard(passengerMutex);
  if (passenger.active && !passenger.exitStatus.has_value()) {
    passenger.exitStatus = 1;
    passengerCv.notify_all();
  }
  idleServicing = false;
}

#ifdef WIN32
BOOL WINAPI TerminalClient::consoleCtrlHandler(DWORD ctrlType) {
  switch (ctrlType) {
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    case CTRL_BREAK_EVENT:
      if (closeOnHangup) {
        requestHangupClose(0);
        waitForHangupClose(3000);
        return TRUE;
      }
      return FALSE;
    default:
      return FALSE;
  }
}
#endif

}  // namespace et
