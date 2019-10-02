#include "TerminalServer.hpp"

#define BUF_SIZE (16 * 1024)

namespace et {
TerminalServer::TerminalServer(
    std::shared_ptr<SocketHandler> _socketHandler,
    const SocketEndpoint &_serverEndpoint,
    std::shared_ptr<PipeSocketHandler> _pipeSocketHandler,
    const SocketEndpoint &_routerEndpoint)
    : ServerConnection(_socketHandler, _serverEndpoint),
      routerEndpoint(_routerEndpoint) {
  terminalRouter = shared_ptr<UserTerminalRouter>(
      new UserTerminalRouter(_pipeSocketHandler, _routerEndpoint));
}

TerminalServer::~TerminalServer() {}

void TerminalServer::run() {
  LOG(INFO) << "Creating server";
  fd_set coreFds;
  int numCoreFds = 0;
  int maxCoreFd = 0;
  FD_ZERO(&coreFds);
  set<int> serverPortFds = socketHandler->getEndpointFds(serverEndpoint);
  for (int i : serverPortFds) {
    FD_SET(i, &coreFds);
    maxCoreFd = max(maxCoreFd, i);
    numCoreFds++;
  }
  FD_SET(terminalRouter->getServerFd(), &coreFds);
  maxCoreFd = max(maxCoreFd, terminalRouter->getServerFd());
  numCoreFds++;

  while (true) {
    {
      lock_guard<std::mutex> guard(terminalThreadMutex);
      if (halt) {
        break;
      }
    }
    // Select blocks until there is something useful to do
    fd_set rfds = coreFds;
    int numFds = numCoreFds;
    int maxFd = maxCoreFd;
    timeval tv;

    if (numFds > FD_SETSIZE) {
      LOG(FATAL) << "Tried to select() on too many FDs";
    }

    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    int numFdsSet = select(maxFd + 1, &rfds, NULL, NULL, &tv);
    FATAL_FAIL(numFdsSet);
    if (numFdsSet == 0) {
      continue;
    }

    // We have something to do!
    for (int i : serverPortFds) {
      if (FD_ISSET(i, &rfds)) {
        acceptNewConnection(i);
      }
    }
    if (FD_ISSET(terminalRouter->getServerFd(), &rfds)) {
      auto idKeyPair = terminalRouter->acceptNewConnection();
      if (idKeyPair.id.length()) {
        addClientKey(idKeyPair.id, idKeyPair.key);
      }
    }
  }

  shutdown();
  {
    lock_guard<std::mutex> guard(terminalThreadMutex);
    halt = true;
  }
  for (auto it : terminalThreads) {
    it->join();
  }
}

void TerminalServer::runJumpHost(
    shared_ptr<ServerClientConnection> serverClientState,
    const InitialPayload &payload) {
  InitialResponse response;
  serverClientState->writePacket(
      Packet(uint8_t(EtPacketType::INITIAL_RESPONSE), protoToString(response)));
  // set thread name
  el::Helpers::setThreadName(serverClientState->getId());
  bool run = true;

  int terminalFd =
      terminalRouter->getInfoForId(serverClientState->getId()).fd();
  shared_ptr<SocketHandler> terminalSocketHandler =
      terminalRouter->getSocketHandler();

  terminalSocketHandler->writePacket(
      terminalFd,
      Packet(TerminalPacketType::JUMPHOST_INIT, protoToString(payload)));

  while (!halt && run && !serverClientState->isShuttingDown()) {
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(terminalFd, &rfd);
    int maxfd = terminalFd;
    int serverClientFd = serverClientState->getSocketFd();
    if (serverClientFd > 0) {
      FD_SET(serverClientFd, &rfd);
      maxfd = max(maxfd, serverClientFd);
    }
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      if (FD_ISSET(terminalFd, &rfd)) {
        try {
          auto packet = terminalSocketHandler->readPacket(terminalFd);
          if (bool(packet)) {
            serverClientState->writePacket(*packet);
          }
        } catch (const std::runtime_error &ex) {
          LOG(INFO) << "Terminal session ended" << ex.what();
          run = false;
          break;
        }
      }

      if (serverClientFd > 0 && FD_ISSET(serverClientFd, &rfd)) {
        VLOG(4) << "Jumphost is selected";
        if (serverClientState->hasData()) {
          VLOG(4) << "Jumphost serverClientState has data";
          Packet packet;
          if (!serverClientState->readPacket(&packet)) {
            continue;
          }
          try {
            terminalSocketHandler->writePacket(terminalFd, packet);
            VLOG(4) << "Jumphost wrote to router " << terminalFd;
          } catch (const std::runtime_error &ex) {
            LOG(INFO) << "Unix socket died between global daemon and terminal "
                         "router: "
                      << ex.what();
            run = false;
            break;
          }
        }
      }
    } catch (const runtime_error &re) {
      LOG(ERROR) << "Jumphost Error: " << re.what();
      cout << "ERROR: " << re.what();
      serverClientState->closeSocket();
    }
  }
  {
    string id = serverClientState->getId();
    serverClientState.reset();
    removeClient(id);
  }
}

void TerminalServer::runTerminal(
    shared_ptr<ServerClientConnection> serverClientState,
    const InitialPayload &payload) {
  auto userInfo = terminalRouter->getInfoForId(serverClientState->getId());
  InitialResponse response;
  shared_ptr<SocketHandler> serverSocketHandler = getSocketHandler();
  shared_ptr<SocketHandler> pipeSocketHandler(new PipeSocketHandler());
  shared_ptr<PortForwardHandler> portForwardHandler(
      new PortForwardHandler(serverSocketHandler, pipeSocketHandler));
  map<string, string> environmentVariables;
  vector<string> pipePaths;
  for (const PortForwardSourceRequest &pfsr : payload.reversetunnels()) {
    string sourceName;
    PortForwardSourceResponse pfsresponse;
    if (pfsr.has_environmentvariable()) {
      pfsresponse = portForwardHandler->createSource(
          pfsr, &sourceName, userInfo.uid(), userInfo.gid());
    } else {
      pfsresponse = portForwardHandler->createSource(
          pfsr, nullptr, userInfo.uid(), userInfo.gid());
    }
    if (pfsresponse.has_error()) {
      InitialResponse response;
      response.set_error(pfsresponse.error());
      serverClientState->writePacket(Packet(
          uint8_t(EtPacketType::INITIAL_RESPONSE), protoToString(response)));
      return;
    }
    if (pfsr.has_environmentvariable()) {
      environmentVariables[pfsr.environmentvariable()] = sourceName;
      pipePaths.push_back(sourceName);
    }
  }
  serverClientState->writePacket(
      Packet(uint8_t(EtPacketType::INITIAL_RESPONSE), protoToString(response)));

  // Set thread name
  el::Helpers::setThreadName(serverClientState->getId());
  // Whether the TE should keep running.
  bool run = true;

  // TE sends/receives data to/from the shell one char at a time.
  char b[BUF_SIZE];

  int terminalFd = userInfo.fd();
  shared_ptr<SocketHandler> terminalSocketHandler =
      terminalRouter->getSocketHandler();

  TermInit termInit;
  for (auto &it : environmentVariables) {
    *(termInit.add_environmentnames()) = it.first;
    *(termInit.add_environmentvalues()) = it.second;
  }
  terminalSocketHandler->writePacket(
      terminalFd,
      Packet(TerminalPacketType::TERMINAL_INIT, protoToString(termInit)));

  while (run) {
    {
      lock_guard<std::mutex> guard(terminalThreadMutex);
      if (halt) {
        break;
      }
    }

    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(terminalFd, &rfd);
    int maxfd = terminalFd;
    int serverClientFd = serverClientState->getSocketFd();
    if (serverClientFd > 0) {
      FD_SET(serverClientFd, &rfd);
      maxfd = max(maxfd, serverClientFd);
    }
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      // Check for data to receive; the received
      // data includes also the data previously sent
      // on the same master descriptor (line 90).
      if (FD_ISSET(terminalFd, &rfd)) {
        // Read from terminal and write to client
        memset(b, 0, BUF_SIZE);
        int rc = read(terminalFd, b, BUF_SIZE);
        if (rc > 0) {
          VLOG(2) << "Sending bytes from terminal: " << rc << " "
                  << serverClientState->getWriter()->getSequenceNumber();
          string s(b, rc);
          et::TerminalBuffer tb;
          tb.set_buffer(s);
          serverClientState->writePacket(
              Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
        } else {
          LOG(INFO) << "Terminal session ended";
          run = false;
          removeClient(serverClientState->getId());
          break;
        }
      }

      vector<PortForwardDestinationRequest> requests;
      vector<PortForwardData> dataToSend;
      portForwardHandler->update(&requests, &dataToSend);
      for (auto &pfr : requests) {
        serverClientState->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST,
                   protoToString(pfr)));
      }
      for (auto &pwd : dataToSend) {
        serverClientState->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DATA, protoToString(pwd)));
      }

      if (serverClientFd > 0 && FD_ISSET(serverClientFd, &rfd)) {
        VLOG(3) << "ServerClientFd is selected";
        while (serverClientState->hasData()) {
          VLOG(3) << "ServerClientState has data";
          Packet packet;
          if (!serverClientState->readPacket(&packet)) {
            break;
          }
          char packetType = packet.getHeader();
          if (packetType == et::TerminalPacketType::PORT_FORWARD_DATA ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE) {
            portForwardHandler->handlePacket(packet, serverClientState);
            continue;
          }
          switch (packetType) {
            case et::TerminalPacketType::TERMINAL_BUFFER: {
              // Read from the server and write to our fake terminal
              et::TerminalBuffer tb =
                  stringToProto<et::TerminalBuffer>(packet.getPayload());
              VLOG(2) << "Got bytes from client: " << tb.buffer().length()
                      << " "
                      << serverClientState->getReader()->getSequenceNumber();
              char c = TERMINAL_BUFFER;
              terminalSocketHandler->writeAllOrThrow(terminalFd, &c,
                                                     sizeof(char), false);
              terminalSocketHandler->writeProto(terminalFd, tb, false);
              break;
            }
            case et::TerminalPacketType::KEEP_ALIVE: {
              // Echo keepalive back to client
              LOG(INFO) << "Got keep alive";
              serverClientState->writePacket(
                  Packet(TerminalPacketType::KEEP_ALIVE, ""));
              break;
            }
            case et::TerminalPacketType::TERMINAL_INFO: {
              LOG(INFO) << "Got terminal info";
              et::TerminalInfo ti =
                  stringToProto<et::TerminalInfo>(packet.getPayload());
              char c = TERMINAL_INFO;
              terminalSocketHandler->writeAllOrThrow(terminalFd, &c,
                                                     sizeof(char), false);
              terminalSocketHandler->writeProto(terminalFd, ti, false);
              break;
            }
            default:
              LOG(FATAL) << "Unknown packet type: " << int(packetType);
          }
        }
      }
    } catch (const runtime_error &re) {
      LOG(ERROR) << "Error: " << re.what();
      cout << "Error: " << re.what();
      serverClientState->closeSocket();
      // If the client disconnects the session, it shouldn't end
      // because the client may be starting a new one.  TODO: Start a
      // timer which eventually kills the server.

      // run=false;
    }
  }
  {
    string id = serverClientState->getId();
    serverClientState.reset();
    removeClient(id);
  }
}

void TerminalServer::handleConnection(
    shared_ptr<ServerClientConnection> serverClientState) {
  Packet packet;
  while (!serverClientState->readPacket(&packet)) {
    LOG(INFO) << "Waiting for initial packet...";
    sleep(1);
  }
  if (packet.getHeader() != EtPacketType::INITIAL_PAYLOAD) {
    LOG(FATAL) << "Invalid header: expecting INITIAL_PAYLOAD but got "
               << packet.getHeader();
  }
  InitialPayload payload = stringToProto<InitialPayload>(packet.getPayload());
  if (payload.jumphost()) {
    LOG(INFO) << "RUNNING JUMPHOST";
    runJumpHost(serverClientState, payload);
  } else {
    LOG(INFO) << "RUNNING TERMINAL";
    runTerminal(serverClientState, payload);
  }
}

bool TerminalServer::newClient(
    shared_ptr<ServerClientConnection> serverClientState) {
  lock_guard<std::mutex> guard(terminalThreadMutex);
  shared_ptr<thread> t = shared_ptr<thread>(
      new thread(&TerminalServer::handleConnection, this, serverClientState));
  terminalThreads.push_back(t);
  return true;
}
}  // namespace et
