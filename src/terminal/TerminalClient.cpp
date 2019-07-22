#include "TerminalClient.hpp"

namespace et {
TerminalClient::TerminalClient(std::shared_ptr<SocketHandler> _socketHandler,
                               const SocketEndpoint& _socketEndpoint,
                               const string& id, const string& passkey,
                               shared_ptr<Console> _console)
    : console(_console), shuttingDown(false) {
  portForwardHandler =
      shared_ptr<PortForwardHandler>(new PortForwardHandler(_socketHandler));
  InitialPayload payload;
  if (_socketEndpoint.isJumphost()) {
    payload.set_jumphost(true);
  }

  connection = shared_ptr<ClientConnection>(
      new ClientConnection(_socketHandler, _socketEndpoint, id, passkey));

  int connectFailCount = 0;
  while (true) {
    try {
      if (connection->connect()) {
        connection->writePacket(
            Packet(EtPacketType::INITIAL_PAYLOAD, protoToString(payload)));
        break;
      } else {
        LOG(ERROR) << "Connecting to server failed: Connect timeout";
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
          LOG(FATAL) << "source/destination port range mismatch";
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
        LOG(FATAL) << "Invalid port range syntax: if source is range, "
                      "destination must be range";
      } else {
        int sourcePort = stoi(sourceDestination[0]);
        int destinationPort = stoi(sourceDestination[1]);
        pairs.push_back(make_pair(sourcePort, destinationPort));
      }
    } catch (const std::logic_error& lr) {
      LOG(FATAL) << "Logic error: " << lr.what();
      exit(1);
    }
  }
  return pairs;
}

void TerminalClient::run(const string& command, const string& tunnels,
                         const string& reverseTunnels) {
  if (console) {
    console->setup();
  }

  shared_ptr<TcpSocketHandler> socketHandler =
      static_pointer_cast<TcpSocketHandler>(connection->getSocketHandler());

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

  try {
    if (tunnels.length()) {
      auto pairs = parseRangesToPairs(tunnels);
      for (auto& pair : pairs) {
        PortForwardSourceRequest pfsr;
        pfsr.set_sourceport(pair.first);
        pfsr.set_destinationport(pair.second);
        auto pfsresponse = portForwardHandler->createSource(pfsr);
        if (pfsresponse.has_error()) {
          throw std::runtime_error(pfsresponse.error());
        }
      }
    }
    if (reverseTunnels.length()) {
      auto pairs = parseRangesToPairs(reverseTunnels);
      for (auto& pair : pairs) {
        PortForwardSourceRequest pfsr;
        pfsr.set_sourceport(pair.first);
        pfsr.set_destinationport(pair.second);

        connection->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_SOURCE_REQUEST,
                   protoToString(pfsr)));
      }
    }
  } catch (const std::runtime_error& ex) {
    cerr << "Error establishing port forward: " << ex.what() << endl;
    LOG(FATAL) << "Error establishing port forward: " << ex.what();
  }

  TerminalInfo lastTerminalInfo;

  if (!console.get()) {
    cerr << "ET running, feel free to background..." << endl;
  }

  while (!shuttingDown && !connection->isShuttingDown()) {
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
          char packetType = packet.getHeader();
          if (packetType == et::TerminalPacketType::PORT_FORWARD_DATA ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_SOURCE_REQUEST ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_SOURCE_RESPONSE ||
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
              LOG(FATAL) << "Unknown packet type: " << int(packetType);
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
          LOG(INFO) << "Window size changed: " << ti.DebugString();
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
      LOG(ERROR) << "Error: " << re.what();
      cout << "Connection closing because of error: " << re.what() << endl;
      shuttingDown = true;
    }
  }
  if (console) {
    console->teardown();
  }
  cout << "Session terminated" << endl;
}
}  // namespace et
