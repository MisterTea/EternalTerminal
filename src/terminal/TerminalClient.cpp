#include "TerminalClient.hpp"

#include "TelemetryService.hpp"
#include "TunnelUtils.hpp"

namespace et {

TerminalClient::TerminalClient(
    shared_ptr<SocketHandler> _socketHandler,
    shared_ptr<SocketHandler> _pipeSocketHandler,
    const SocketEndpoint& _socketEndpoint, const string& id,
    const string& passkey, shared_ptr<Console> _console, bool jumphost,
    const string& tunnels, const string& reverseTunnels, bool forwardSshAgent,
    const string& identityAgent, int _keepaliveDuration)
    : console(_console),
      shuttingDown(false),
      keepaliveDuration(_keepaliveDuration) {
  portForwardHandler = shared_ptr<PortForwardHandler>(
      new PortForwardHandler(_socketHandler, _pipeSocketHandler));
  InitialPayload payload;
  payload.set_jumphost(jumphost);

  try {
    if (tunnels.length()) {
      auto pfsrs = parseRangesToRequests(tunnels);
      for (auto& pfsr : pfsrs) {
#ifdef WIN32
        STFATAL << "Source tunnel not supported on windows yet";
#else
        auto pfsresponse =
            portForwardHandler->createSource(pfsr, nullptr, -1, -1);
        if (pfsresponse.has_error()) {
          throw std::runtime_error(pfsresponse.error());
        }
#endif
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
        fd_set rfd;
        timeval tv;
        for (int a = 0; a < 3; a++) {
          FD_ZERO(&rfd);
          int clientFd = connection->getSocketFd();
          if (clientFd < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
          }
          FD_SET(clientFd, &rfd);
          tv.tv_sec = 1;
          tv.tv_usec = 0;
          select(clientFd + 1, &rfd, NULL, NULL, &tv);
          if (FD_ISSET(clientFd, &rfd)) {
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

void TerminalClient::run(const string& command) {
  if (console) {
    console->setup();
  }

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  time_t keepaliveTime = time(NULL) + keepaliveDuration;
  bool waitingOnKeepalive = false;

  if (command.length()) {
    LOG(INFO) << "Got command: " << command;
    et::TerminalBuffer tb;
    tb.set_buffer(command + "; exit\n");

    connection->writePacket(
        Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
  }

  TerminalInfo lastTerminalInfo;

  if (!console.get()) {
    // NOTE: ../../scripts/ssh-et relies on the wording of this message, so if
    // you change it please update it as well.
    CLOG(INFO, "stdout") << "ET running, feel free to background..." << endl;
  }

  while (!connection->isShuttingDown()) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    int maxfd = -1;
    int consoleFd = -1;
    if (console) {
      consoleFd = console->getFd();
      maxfd = consoleFd;
      FD_SET(consoleFd, &rfd);
    }
    int clientFd = connection->getSocketFd();
    if (clientFd > 0) {
      FD_SET(clientFd, &rfd);
      maxfd = max(maxfd, clientFd);
    }
    // TODO: set port forward sockets as well for performance reasons.
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      if (console) {
        // Check for data to send.
        if (FD_ISSET(consoleFd, &rfd)) {
          // Read from stdin and write to our client that will then send it to
          // the server.
          VLOG(4) << "Got data from stdin";
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
              et::TerminalBuffer tb;
              tb.set_buffer(s);

              connection->writePacket(Packet(
                  TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
              keepaliveTime = time(NULL) + keepaliveDuration;
            }
          }
#else
          if (console) {
            int rc = ::read(consoleFd, b, BUF_SIZE);
            FATAL_FAIL(rc);
            if (rc > 0) {
              // VLOG(1) << "Sending byte: " << int(b) << " " << char(b) << " "
              // << connection->getWriter()->getSequenceNumber();
              string s(b, rc);
              et::TerminalBuffer tb;
              tb.set_buffer(s);

              connection->writePacket(Packet(
                  TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
              keepaliveTime = time(NULL) + keepaliveDuration;
            }
          }
#endif
        }
      }

      if (clientFd > 0 && FD_ISSET(clientFd, &rfd)) {
        VLOG(4) << "Clientfd is selected";
        while (connection->hasData()) {
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
              if (console) {
                VLOG(3) << "Got terminal buffer";
                // Read from the server and write to our fake terminal
                et::TerminalBuffer tb =
                    stringToProto<et::TerminalBuffer>(packet.getPayload());
                const string& s = tb.buffer();
                // VLOG(5) << "Got message: " << s;
                // VLOG(1) << "Got byte: " << int(b) << " " << char(b) << " " <<
                // connection->getReader()->getSequenceNumber();
                keepaliveTime = time(NULL) + keepaliveDuration;
                console->write(s);
              }
              break;
            }
            case et::TerminalPacketType::KEEP_ALIVE:
              waitingOnKeepalive = false;
              // This will fill up log file quickly but is helpful for debugging
              // latency issues.
              LOG(INFO) << "Got a keepalive";
              break;
            default:
              STFATAL << "Unknown packet type: " << int(packetType);
          }
        }
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
        TerminalInfo ti = console->getTerminalInfo();

        if (ti != lastTerminalInfo) {
          LOG(INFO) << "Window size changed: row: " << ti.row()
                    << " column: " << ti.column() << " width: " << ti.width()
                    << " height: " << ti.height();
          lastTerminalInfo = ti;
          connection->writePacket(
              Packet(TerminalPacketType::TERMINAL_INFO, protoToString(ti)));
        }
      }

      vector<PortForwardDestinationRequest> requests;
      vector<PortForwardData> dataToSend;
      portForwardHandler->update(&requests, &dataToSend);
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
  if (console) {
    console->teardown();
  }
  CLOG(INFO, "stdout") << "Session terminated" << endl;
}
}  // namespace et
