#include "TerminalClient.hpp"

namespace et {
vector<pair<int, int>> parseRangesToPairs(const string& input) {
  vector<pair<int, int>> pairs;
  auto j = split(input, ',');
  for (auto& pair : j) {
    vector<string> sourceDestination = split(pair, ':');
    try {
      if (sourceDestination[0].find('-') != string::npos &&
          sourceDestination[1].find('-') != string::npos) {
        vector<string> sourcePortRange = split(sourceDestination[0], '-');
        int sourcePortStart = stoi(sourcePortRange[0]);
        int sourcePortEnd = stoi(sourcePortRange[1]);

        vector<string> destinationPortRange = split(sourceDestination[1], '-');
        int destinationPortStart = stoi(destinationPortRange[0]);
        int destinationPortEnd = stoi(destinationPortRange[1]);

        if (sourcePortEnd - sourcePortStart !=
            destinationPortEnd - destinationPortStart) {
          STFATAL << "source/destination port range mismatch";
          exit(1);
        } else {
          int portRangeLength = sourcePortEnd - sourcePortStart + 1;
          for (int i = 0; i < portRangeLength; ++i) {
            pairs.push_back(
                make_pair(sourcePortStart + i, destinationPortStart + i));
          }
        }
      } else if (sourceDestination[0].find('-') != string::npos ||
                 sourceDestination[1].find('-') != string::npos) {
        STFATAL << "Invalid port range syntax: if source is range, "
                   "destination must be range";
      } else {
        int sourcePort = stoi(sourceDestination[0]);
        int destinationPort = stoi(sourceDestination[1]);
        pairs.push_back(make_pair(sourcePort, destinationPort));
      }
    } catch (const std::logic_error& lr) {
      STFATAL << "Logic error: " << lr.what();
      exit(1);
    }
  }
  return pairs;
}

TerminalClient::TerminalClient(shared_ptr<SocketHandler> _socketHandler,
                               shared_ptr<SocketHandler> _pipeSocketHandler,
                               const SocketEndpoint& _socketEndpoint,
                               const string& id, const string& passkey,
                               shared_ptr<Console> _console, bool jumphost,
                               const string& tunnels,
                               const string& reverseTunnels,
                               bool forwardSshAgent,
                               const string& identityAgent)
    : console(_console), shuttingDown(false) {
  portForwardHandler = shared_ptr<PortForwardHandler>(
      new PortForwardHandler(_socketHandler, _pipeSocketHandler));
  InitialPayload payload;
  payload.set_jumphost(jumphost);

  try {
    if (tunnels.length()) {
      auto pairs = parseRangesToPairs(tunnels);
      for (auto& pair : pairs) {
        PortForwardSourceRequest pfsr;
        pfsr.mutable_source()->set_port(pair.first);
        pfsr.mutable_destination()->set_port(pair.second);
        auto pfsresponse =
            portForwardHandler->createSource(pfsr, nullptr, -1, -1);
        if (pfsresponse.has_error()) {
          throw std::runtime_error(pfsresponse.error());
        }
      }
    }
    if (reverseTunnels.length()) {
      auto pairs = parseRangesToPairs(reverseTunnels);
      for (auto& pair : pairs) {
        PortForwardSourceRequest pfsr;
        pfsr.mutable_source()->set_port(pair.first);
        pfsr.mutable_destination()->set_port(pair.second);
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
          cout << "Missing environment variable SSH_AUTH_SOCK.  Are you sure "
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
    cout << "Error establishing port forward: " << ex.what() << endl;
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
            sleep(1);
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
                cout << "Error: Missing initial response\n";
                STFATAL << "Missing initial response!";
              }
              auto initialResponse = stringToProto<InitialResponse>(
                  initialResponsePacket.getPayload());
              if (initialResponse.has_error()) {
                cout << "Error initializing connection: "
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
        STERROR << "Connecting to server failed: Connect timeout";
        connectFailCount++;
        if (connectFailCount == 3) {
          throw std::runtime_error("Connect Timeout");
        }
      }
    } catch (const runtime_error& err) {
      LOG(INFO) << "Could not make initial connection to server";
      cout << "Could not make initial connection to " << _socketEndpoint << ": "
           << err.what() << endl;
      exit(1);
    }
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

  time_t keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
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
    cout << "ET running, feel free to background..." << endl;
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
          int rc = read(consoleFd, b, BUF_SIZE);
          FATAL_FAIL(rc);
          if (rc > 0) {
            // VLOG(1) << "Sending byte: " << int(b) << " " << char(b) << " " <<
            // connection->getWriter()->getSequenceNumber();
            string s(b, rc);
            et::TerminalBuffer tb;
            tb.set_buffer(s);

            connection->writePacket(
                Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
            keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
          }
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
            keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
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
                keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
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
        keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
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
        keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
      }
      for (auto& pwd : dataToSend) {
        connection->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DATA, protoToString(pwd)));
        VLOG(4) << "send PF data";
        keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
      }
    } catch (const runtime_error& re) {
      STERROR << "Error: " << re.what();
      cout << "Connection closing because of error: " << re.what() << endl;
      lock_guard<recursive_mutex> guard(shutdownMutex);
      shuttingDown = true;
    }
  }
  if (console) {
    console->teardown();
  }
  cout << "Session terminated" << endl;
}
}  // namespace et
