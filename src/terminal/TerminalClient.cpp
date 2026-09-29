#include "TerminalClient.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "PlatformUtils.hpp"
#include "PollSet.hpp"
#include "RawSocketUtils.hpp"
#include "SocksUtils.hpp"
#include "TelemetryService.hpp"
#include "TmuxCcFilter.hpp"
#include "TunnelUtils.hpp"
#include "WriteBuffer.hpp"

namespace et {
string refreshAgentProxyPath(const string& id, const string& authSock) {
  const fs::path directory = fs::path(GetTempDirectory()) / ("et-agent-" + id);
  std::error_code error;
  fs::create_directories(directory, error);
  if (error) {
#ifdef WIN32
    return authSock;
#else
    throw runtime_error("Unable to create SSH agent proxy directory: " +
                        error.message());
#endif
  }
#ifndef WIN32
  if (::chmod(directory.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
    throw runtime_error("Unable to secure SSH agent proxy directory");
  }
#endif
  // Create under a temp name then rename over the stable path so readers never
  // observe a missing agent.sock between remove and recreate (Issue #506).
  const fs::path proxy = directory / "agent.sock";
  const fs::path proxyTmp =
      directory / ("agent.sock.tmp." + genRandomAlphaNum(8));
  fs::create_symlink(authSock, proxyTmp, error);
  if (error) {
    fs::remove(proxyTmp, error);
#ifdef WIN32
    return authSock;
#else
    throw runtime_error("Unable to refresh SSH agent proxy: " +
                        error.message());
#endif
  }
  error.clear();
  fs::rename(proxyTmp, proxy, error);
  if (error) {
    // Some platforms refuse rename-over-existing for symlinks; fall back.
    std::error_code removeError;
    fs::remove(proxy, removeError);
    error.clear();
    fs::rename(proxyTmp, proxy, error);
  }
  if (error) {
    fs::remove(proxyTmp, error);
#ifdef WIN32
    return authSock;
#else
    throw runtime_error("Unable to refresh SSH agent proxy: " +
                        error.message());
#endif
  }
  string proxyPath = proxy.string();
#ifdef WIN32
  for (char& c : proxyPath) {
    if (c == '\\') {
      c = '/';
    }
  }
#endif
  return proxyPath;
}

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

// Restores the console when run() exits early, including by exception.
class ConsoleSetupGuard {
 public:
  explicit ConsoleSetupGuard(const shared_ptr<Console>& console)
      : console_(console) {}

  void setup() {
    if (console_) {
      shouldTeardown_ = true;
      console_->setup();
    }
  }

  void teardown() {
    if (shouldTeardown_) {
      shouldTeardown_ = false;
      console_->teardown();
    }
  }

  ~ConsoleSetupGuard() { teardown(); }

 private:
  shared_ptr<Console> console_;
  bool shouldTeardown_ = false;
};
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
    std::function<bool(const string&)> _sessionTitleUpdate,
    optional<int> disconnectTimeoutMinutes, bool noShell,
    bool exitOnForwardFailure)
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
  if (stdioForwardActive || noShell) {
    payload.set_no_shell(true);
  } else if (noPty) {
    payload.set_no_pty(true);
    payload.set_command(command);
  }
  if (disconnectTimeoutMinutes) {
    // Overflow already rejected in TerminalClientMain; convert minutes →
    // seconds.
    payload.set_disconnect_timeout_seconds(*disconnectTimeoutMinutes * 60);
  }

  for (const auto& envVar : envVars) {
    (*payload.mutable_environmentvariables())[envVar.first] = envVar.second;
  }

  try {
    auto failForward = [&](const string& message) {
      if (exitOnForwardFailure) {
        throw std::runtime_error(message);
      }
      LOG(WARNING) << message;
    };
    if (tunnels.length()) {
      auto pfsrs = parseRangesToRequests(tunnels);
      for (auto& pfsr : pfsrs) {
        auto pfsresponse =
            portForwardHandler->createSource(pfsr, nullptr, -1, -1);
        if (pfsresponse.has_error()) {
          std::ostringstream failed;
          failed << "Failed to establish port forward " << pfsr.source()
                 << " -> " << pfsr.destination() << " - "
                 << pfsresponse.error();
          failForward(failed.str());
          continue;
        }
      }
    }
    for (const auto& dynamicArg : dynamicForwards) {
      SocketEndpoint socksSource = parseDynamicForwardArg(dynamicArg);
      auto response = portForwardHandler->createSocksSource(socksSource);
      if (response.has_error()) {
        failForward("Failed to establish dynamic forward " + dynamicArg +
                    " - " + response.error());
        continue;
      }
    }
    if (stdioForwardActive) {
#ifdef WIN32
      // CRT fds 0/1 are not sockets. The Windows poller ignores fd 0 and
      // SocketHandler uses recv/send, so -W cannot bridge stdio yet.
      throw std::runtime_error(
          "-W/--stdio-forward is not supported on Windows");
#else
      SocketEndpoint destination = parseStdioForwardArg(stdioForward);
      auto response = portForwardHandler->createStdioForward(
          destination, STDIN_FILENO, STDOUT_FILENO, false);
      if (response.has_error()) {
        throw std::runtime_error("Failed to establish stdio forward - " +
                                 response.error());
      }
#endif
    }
    if (reverseTunnels.length()) {
      auto pfsrs = parseRangesToRequests(reverseTunnels);
      for (auto& pfsr : pfsrs) {
        *(payload.add_reversetunnels()) = pfsr;
      }
    }
    // Resolve the current agent socket once for both fresh forward and
    // saved-session reattach. Returning clients keep the server-side reverse
    // tunnel destination from the first InitialPayload; we only retarget the
    // stable local proxy symlink.
    auto resolveAuthSock = [&](bool requireAuthSock) -> string {
      if (identityAgent.length()) {
        return identityAgent;
      }
      auto authSockEnv = getenv("SSH_AUTH_SOCK");
      if (!authSockEnv) {
        if (requireAuthSock) {
          throw std::runtime_error(
              "Missing environment variable SSH_AUTH_SOCK. Are you sure you "
              "ran ssh-agent first?");
        }
        return "";
      }
      return string(authSockEnv);
    };
    if (forwardSshAgent) {
      string authSock = resolveAuthSock(/*requireAuthSock=*/true);
      if (authSock.length()) {
        PortForwardSourceRequest pfsr;
        pfsr.mutable_destination()->set_name(
            refreshAgentProxyPath(id, authSock));
        pfsr.set_environmentvariable("SSH_AUTH_SOCK");
        *(payload.add_reversetunnels()) = pfsr;
        agentProxyEnabled = true;
        agentClientId = id;
        agentIdentityAgent = identityAgent;
      }
    } else if (_resumeSavedSession) {
      // attachSavedSession forbids --forward-ssh-agent, but the server still
      // holds the original reverse-tunnel destination. Retarget the proxy.
      string authSock = resolveAuthSock(/*requireAuthSock=*/false);
      if (authSock.length()) {
        refreshAgentProxyPath(id, authSock);
        agentProxyEnabled = true;
        agentClientId = id;
        agentIdentityAgent = identityAgent;
      }
    }
  } catch (const std::runtime_error& ex) {
    throw std::runtime_error(string("Error establishing port forward: ") +
                             ex.what());
  }

  connection = shared_ptr<ClientConnection>(
      new ClientConnection(_socketHandler, _socketEndpoint, id, passkey,
                           /*_resetIntent=*/_resumeSavedSession));
  if (agentProxyEnabled) {
    connection->setPostReconnectCallback([this]() { refreshAgentProxy(); });
  }

  int connectFailCount = 0;
  bool connected = false;
  bool payloadSent = false;
  while (true) {
    try {
      bool fail = true;
      // Retry connect() only when it failed. A live socket whose
      // INITIAL_RESPONSE is slow must stay up: calling connect() again is a
      // RETURNING_CLIENT reconnect and would resend INITIAL_PAYLOAD.
      if (!connected) {
        connected = connection->connect();
      }
      if (connected) {
        if (connection->wasRecovered()) {
          // Reattached: the session was bootstrapped when it started.
          fail = false;
        } else {
          if (!payloadSent) {
            connection->writePacket(
                Packet(EtPacketType::INITIAL_PAYLOAD, protoToString(payload)));
            payloadSent = true;
          }
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
                  throw std::runtime_error("Error initializing connection: " +
                                           initialResponse.error());
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
      // The destructor does not run when the constructor throws.
      connection->shutdown();
      if (_resumeSavedSession &&
          connection->lastStatus() == et::ConnectStatus::INVALID_KEY) {
        throw std::runtime_error(INVALID_SESSION_CONNECT_ERROR);
      }
      if (!_resumeSavedSession) {
        std::ostringstream message;
        message << "Could not make initial connection to " << _socketEndpoint
                << ": " << err.what();
        throw std::runtime_error(message.str());
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

void TerminalClient::refreshAgentProxy() {
  if (!agentProxyEnabled) {
    return;
  }
  string authSock = agentIdentityAgent;
  if (authSock.empty()) {
    auto authSockEnv = getenv("SSH_AUTH_SOCK");
    if (!authSockEnv) {
      LOG(WARNING) << "SSH_AUTH_SOCK unset; leaving agent proxy unchanged";
      return;
    }
    authSock.assign(authSockEnv);
  }
  try {
    refreshAgentProxyPath(agentClientId, authSock);
  } catch (const std::exception& ex) {
    LOG(WARNING) << "Unable to refresh SSH agent proxy: " << ex.what();
  }
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
  ConsoleSetupGuard consoleSetupGuard(console);
  consoleSetupGuard.setup();

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
    const bool readConsole = console && !consoleInputDisabled;
    vector<int> consoleInputFds;
    if (readConsole) {
      consoleInputFds = console->getInputPollFds();
    }
    const bool consoleWritable = console && consoleOut.hasPendingData();
    const int clientFd = connection->getSocketFd();
    const bool watchClient =
        clientFd > 0 && consoleOut.size() < WriteBuffer::FLUSH_THRESHOLD;
    // Include port forward sockets for low-latency forwarding.
    set<int> pfFds;
    portForwardHandler->getForwardFds(&pfFds);

    PollSet pollSet;
    for (int fd : consoleInputFds) {
      pollSet.watch(fd, true, false);
    }
    if (consoleWritable) {
      // Only needs to wake the loop; the drain below re-checks writability.
      pollSet.watch(console->getOutputPollFd(), false, true);
    }
    if (watchClient) {
      pollSet.watch(clientFd, true, false);
    }
    for (int fd : pfFds) {
      pollSet.watch(fd, true, false);
    }
    const set<int> readyFds = pollSet.waitReadable(10);

    try {
      bool skipServerRead = false;
      bool inputReady = readConsole && consoleInputFds.empty();
      for (int fd : consoleInputFds) {
        inputReady = inputReady || readyFds.count(fd) != 0;
      }
      if (inputReady) {
        VLOG(4) << "Got data from stdin";
        string s;
        const ConsoleInputStatus status = console->readInput(readyFds, &s);
        if (status == ConsoleInputStatus::FAILED) {
          break;
        }
        if (status == ConsoleInputStatus::CLOSED) {
          consoleInputDisabled = true;
        }
        if (status == ConsoleInputStatus::DATA && !s.empty()) {
          et::TerminalBuffer tb;
          tb.set_buffer(s);

          connection->writePacket(
              Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
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
                WriteToStdStream(STDERR_FILENO, tb.buffer().data(),
                                 tb.buffer().size());
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
                WriteToStdStream(STDOUT_FILENO, tb.buffer().data(),
                                 tb.buffer().size());
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
      portForwardHandler->update(&requests, &dataToSend, &readyFds);
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
      PollSet writable;
      writable.watch(console->getOutputPollFd(), false, true);
      writable.waitReadable(10);
    }
    consoleSetupGuard.teardown();
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
    PollSet pollSet;
    pollSet.watch(passengerInFd, true, false);
    if (passengerOut.hasPendingData()) {
      pollSet.watch(passengerOutFd, false, true);
    }
    if (clientFd > 0) {
      pollSet.watch(clientFd, true, false);
    }
    for (int fd : pfFds) {
      pollSet.watch(fd, true, false);
    }
    const set<int> readyFds = pollSet.waitReadable(50);

    try {
      if (passengerInFd >= 0 && readyFds.count(passengerInFd)) {
        char b[4096];
        const ssize_t rc =
            RawSocketUtils::readSome(passengerInFd, b, sizeof(b));
        const int readErrno = GetErrno();
        if (rc > 0) {
          et::TerminalBuffer tb;
          tb.set_buffer(string(b, rc));
          connection->writePacket(
              Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
          keepaliveTime = time(NULL) + keepaliveDuration;
        } else if (rc == 0 ||
                   (readErrno != EAGAIN && readErrno != EWOULDBLOCK &&
                    readErrno != EINTR)) {
          // Mirror primary run(): disable further local input; keep bridging
          // until a real session-end signal (exit marker / TERMINAL_CLOSE /
          // idle teardown).
          passengerInputDisabled = true;
        }
      }

      if (clientFd > 0) {
        bool haveData = readyFds.count(clientFd) != 0 || connection->hasData();
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

      if (passengerOutFd >= 0 && passengerOut.hasPendingData()) {
        size_t count = 0;
        const char* data = passengerOut.peekData(&count);
        if (data != nullptr && count > 0) {
          const ssize_t written =
              RawSocketUtils::writeSome(passengerOutFd, data, count);
          if (written > 0) {
            passengerOut.consume(static_cast<size_t>(written));
          }
        }
      }

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
      portForwardHandler->update(&requests, &dataToSend, &readyFds);
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
