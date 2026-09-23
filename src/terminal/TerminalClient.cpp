#include "TerminalClient.hpp"

#include <cstdint>

#include "PseudoTerminalConsole.hpp"
#include "RawSocketUtils.hpp"
#include "TelemetryService.hpp"
#include "TmuxCcFilter.hpp"
#include "TunnelUtils.hpp"
#include "WriteBuffer.hpp"

namespace et {
std::atomic<bool> TerminalClient::closeOnHangup(false);
std::atomic<bool> TerminalClient::hangupCloseRequested(false);
std::atomic<bool> TerminalClient::hangupCloseCompleted(false);

TerminalClient::TerminalClient(
    shared_ptr<SocketHandler> _socketHandler,
    shared_ptr<SocketHandler> _pipeSocketHandler,
    const SocketEndpoint& _socketEndpoint, const string& id,
    const string& passkey, shared_ptr<Console> _console, bool jumphost,
    const string& tunnels, const string& reverseTunnels, bool forwardSshAgent,
    const string& identityAgent, int _keepaliveDuration,
    const vector<pair<string, string>>& envVars, bool _noPty,
    const string& command)
    : console(_console),
      shuttingDown(false),
      keepaliveDuration(_keepaliveDuration),
      noPty(_noPty) {
  portForwardHandler = shared_ptr<PortForwardHandler>(
      new PortForwardHandler(_socketHandler, _pipeSocketHandler));
  InitialPayload payload;
  payload.set_jumphost(jumphost);
  payload.set_supports_exit_status(true);
  if (noPty) {
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
      new ClientConnection(_socketHandler, _socketEndpoint, id, passkey));

  int connectFailCount = 0;
  while (true) {
    try {
      bool fail = true;
      if (connection->connect()) {
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
      if (fail) {
        LOG(WARNING) << "Connecting to server failed: Connect timeout";
        connectFailCount++;
        if (connectFailCount == 3) {
          throw std::runtime_error("Connect Timeout");
        }
      }
    } catch (const runtime_error& err) {
      LOG(INFO) << "Could not make initial connection to server";
      CLOG(INFO, "stdout") << "Could not make initial connection to "
                           << _socketEndpoint << ": " << err.what() << endl;
      exit(1);
    }

    TelemetryService::get()->logToDatadog("Connection Established",
                                          el::Level::Info, __FILE__, __LINE__);
    break;
  }
  VLOG(1) << "Client created with id: " << connection->getId();
};

TerminalClient::~TerminalClient() {
  connection->shutdown();
  console.reset();
  portForwardHandler.reset();
  connection.reset();
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

  if (!console.get()) {
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
  // writable.
  if (console) {
    while (consoleOut.hasPendingData()) {
      size_t count = 0;
      const char* data = consoleOut.peekData(&count);
      if (data == nullptr || count == 0) {
        break;
      }
      size_t written = console->writeSome(string(data, count));
      if (written > 0) {
        consoleOut.consume(written);
        continue;
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
  CLOG(INFO, "stdout") << "Session terminated" << endl;
  if (wantRemoteExitStatus && haveRemoteExitStatus) {
    return remoteExitStatus;
  }
  return 0;
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
