#ifndef WIN32
#include "TerminalServer.hpp"

#include "TelemetryService.hpp"
#include "WriteBuffer.hpp"

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

  if (TelemetryService::exists()) {
    TelemetryService::get()->logToDatadog("Server started", el::Level::Info,
                                          __FILE__, __LINE__);
  }

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
      STFATAL << "Tried to select() on too many FDs";
    }

    tv.tv_sec = 0;
    tv.tv_usec = 100000;

    const int numFdsSet = select(maxFd + 1, &rfds, NULL, NULL, &tv);
    if (numFdsSet < 0 && errno == EINTR) {
      // If EINTR was returned, then the syscall was interrupted by a signal.
      // This is not an error, but can be a signal that the program is being
      // shutdown, so restart the loop to check for the halt condition.
      continue;
    }

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

  int terminalFd = -1;
  if (auto maybeUserInfo =
          terminalRouter->tryGetInfoForConnection(serverClientState)) {
    terminalFd = maybeUserInfo->fd();
  } else {
    LOG(ERROR) << "Jumphost failed to bind to terminal router";
    serverClientState->closeSocket();
    return;
  }

  shared_ptr<SocketHandler> terminalSocketHandler =
      terminalRouter->getSocketHandler();

  terminalSocketHandler->writePacket(
      terminalFd,
      Packet(TerminalPacketType::JUMPHOST_INIT, protoToString(payload)));

  // Flow control: buffer for pending jumphost output to client
  std::deque<Packet> pendingPackets;
  size_t pendingBytes = 0;
  const size_t MAX_PENDING_BYTES = 256 * 1024;  // 256KB limit

  while (true) {
    {
      lock_guard<std::mutex> guard(terminalThreadMutex);
      if (halt || !run || serverClientState->isShuttingDown()) {
        break;
      }
    }

    fd_set rfd, wfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_ZERO(&wfd);

    // Only read from terminal if we have room in the buffer
    if (pendingBytes < MAX_PENDING_BYTES) {
      FD_SET(terminalFd, &rfd);
    }

    int maxfd = terminalFd;
    int serverClientFd = serverClientState->getSocketFd();
    if (serverClientFd > 0) {
      FD_SET(serverClientFd, &rfd);
      maxfd = max(maxfd, serverClientFd);

      // Monitor write availability if we have pending packets
      if (!pendingPackets.empty()) {
        FD_SET(serverClientFd, &wfd);
      }
    }
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    select(maxfd + 1, &rfd, &wfd, NULL, &tv);

    try {
      // First, drain pending packets when socket is writable
      if (serverClientFd > 0 && FD_ISSET(serverClientFd, &wfd) &&
          !pendingPackets.empty()) {
        while (!pendingPackets.empty()) {
          serverClientState->writePacket(pendingPackets.front());
          pendingBytes -= pendingPackets.front().length();
          pendingPackets.pop_front();

          // Check if socket is still writable for more writes
          if (!waitOnSocketWritable(serverClientFd)) {
            break;
          }
        }
      }

      // Read from terminal if buffer has room
      if (FD_ISSET(terminalFd, &rfd) && pendingBytes < MAX_PENDING_BYTES) {
        try {
          Packet packet;
          if (terminalSocketHandler->readPacket(terminalFd, &packet)) {
            pendingPackets.push_back(packet);
            pendingBytes += packet.length();
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
      STERROR << "Jumphost Error: " << re.what();
      CLOG(INFO, "stdout") << "ERROR: " << re.what();
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
  auto maybeUserInfo =
      terminalRouter->tryGetInfoForConnection(serverClientState);
  if (!maybeUserInfo) {
    LOG(ERROR) << "Terminal client failed to bind to terminal router";
    serverClientState->closeSocket();
    return;
  }

  const auto userInfo = std::move(maybeUserInfo.value());

  InitialResponse response;
  shared_ptr<SocketHandler> serverSocketHandler = getSocketHandler();
  shared_ptr<SocketHandler> pipeSocketHandler(new PipeSocketHandler());
  shared_ptr<PortForwardHandler> portForwardHandler(
      new PortForwardHandler(serverSocketHandler, pipeSocketHandler));
  map<string, string> environmentVariables;

  for (const auto &envVar : payload.environmentvariables()) {
    environmentVariables[envVar.first] = envVar.second;
    LOG(INFO) << "SetEnv: " << envVar.first << "=" << envVar.second;
  }

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

  // Flow control: buffer for pending terminal output to client
  // This creates backpressure when the client is slow to consume data
  WriteBuffer terminalOutputBuffer;

  while (run) {
    {
      lock_guard<std::mutex> guard(terminalThreadMutex);
      if (halt) {
        break;
      }
    }

    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd, wfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_ZERO(&wfd);

    // Only read from terminal if we have room in the output buffer
    // This is key for backpressure: if client is slow, we stop reading
    if (terminalOutputBuffer.canAcceptMore()) {
      FD_SET(terminalFd, &rfd);
    }

    int maxfd = terminalFd;
    int serverClientFd = serverClientState->getSocketFd();
    if (serverClientFd > 0) {
      FD_SET(serverClientFd, &rfd);
      maxfd = max(maxfd, serverClientFd);

      // Monitor write availability if we have pending data
      if (terminalOutputBuffer.hasPendingData()) {
        FD_SET(serverClientFd, &wfd);
      }
    }
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    select(maxfd + 1, &rfd, &wfd, NULL, &tv);

    try {
      // First, try to drain the output buffer when socket is writable
      // This should be done before reading more data
      if (serverClientFd > 0 && FD_ISSET(serverClientFd, &wfd) &&
          terminalOutputBuffer.hasPendingData()) {
        // Drain as much as possible from the buffer
        while (terminalOutputBuffer.hasPendingData()) {
          size_t count;
          const char *data = terminalOutputBuffer.peekData(&count);
          if (data == nullptr || count == 0) break;

          // Create a TerminalBuffer packet and send it
          string s(data, count);
          et::TerminalBuffer tb;
          tb.set_buffer(s);
          VLOG(2) << "Draining buffered bytes to client: " << count << " "
                  << serverClientState->getWriter()->getSequenceNumber();
          serverClientState->writePacket(
              Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
          terminalOutputBuffer.consume(count);

          // Check if socket is still writable for more writes
          if (!waitOnSocketWritable(serverClientFd)) {
            break;  // Socket would block, stop draining
          }
        }
      }

      // Check for data to receive from terminal
      // Only if we have room in the buffer (backpressure)
      if (FD_ISSET(terminalFd, &rfd) && terminalOutputBuffer.canAcceptMore()) {
        // Read from terminal and buffer for later write to client
        memset(b, 0, BUF_SIZE);
        int rc = read(terminalFd, b, BUF_SIZE);
        if (rc > 0) {
          VLOG(2) << "Read bytes from terminal: " << rc
                  << " buffer size: " << terminalOutputBuffer.size();
          // Buffer the data for flow-controlled sending
          terminalOutputBuffer.enqueue(string(b, rc));
        } else if (rc == 0) {
          LOG(INFO) << "Terminal session ended";
          run = false;
          break;
        } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
          LOG(INFO) << "Socket temporarily unavailable, trying again...";
          sleep(1);
          continue;
        } else {
          LOG(ERROR) << "Error reading from socket: " << errno << " "
                     << strerror(errno);
          run = false;
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
          uint8_t packetType = packet.getHeader();
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
              STFATAL << "Unknown packet type: " << int(packetType);
          }
        }
      }
    } catch (const runtime_error &re) {
      STERROR << "Error: " << re.what();
      CLOG(INFO, "stdout") << "Error: " << re.what();
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
    STFATAL << "Invalid header: expecting INITIAL_PAYLOAD but got "
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
#endif
