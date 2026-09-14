#ifndef WIN32
#include "TerminalServer.hpp"

#include <cstdint>

#include "FdPoller.hpp"
#include "JumphostPending.hpp"
#include "TelemetryService.hpp"
#include "TmuxCcFilter.hpp"
#include "WriteBuffer.hpp"

#define BUF_SIZE (16 * 1024)

namespace et {
namespace {

void drainDiscardReadableBytes(int fd, WriteBuffer* buf) {
  char bufBytes[BUF_SIZE];
  bool got = false;
  while (true) {
    if (!waitOnSocketData(fd, 0, 0)) {
      break;
    }
    int rc = ::read(fd, bufBytes, BUF_SIZE);
    if (rc <= 0) {
      break;
    }
    buf->enqueue(string(bufBytes, rc));
    got = true;
  }
  if (got) {
    buf->filterDroppable();
  }
}

// Returns true when control notifications were just sent and a large
// droppable backlog remains: the caller should wait for client input
// (send-keys) before writing more pane output.
bool drainWriteBufferToClient(WriteBuffer* buf,
                              shared_ptr<ServerClientConnection> conn,
                              int serverClientFd) {
  if (!buf->hasPendingData()) {
    return false;
  }
  buf->promoteControlLines();
  const size_t controlBefore = buf->controlBytesAtFront();
  if (serverClientFd > 0) {
    while (buf->hasPendingData()) {
      if (conn->hasData()) {
        return false;
      }
      if (!isSocketWritable(serverClientFd)) {
        break;
      }
      size_t count = 0;
      const char* data = buf->peekData(&count);
      if (data == nullptr || count == 0) {
        break;
      }
      const bool droppable = buf->controlBytesAtFront() == 0;
      if (droppable && controlBefore > 0 && buf->shouldFlushOnInterrupt()) {
        LOG(INFO) << "Holding " << buf->size()
                  << " droppable bytes for client interrupt";
        return true;
      }
      if (buf->controlBytesAtFront() > 0) {
        count = min(count, buf->controlBytesAtFront());
      } else {
        count = min(count, size_t(BUF_SIZE));
      }
      et::TerminalBuffer tb;
      tb.set_buffer(string(data, count));
      conn->writePacket(
          Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
      buf->consume(count);
    }
    return false;
  }
  // Disconnected: hand leftover output to BackedWriter (64MB cap).
  while (buf->hasPendingData()) {
    if (!conn->canBufferWrite(2 * BUF_SIZE)) {
      break;
    }
    size_t count = 0;
    const char* data = buf->peekData(&count);
    if (data == nullptr || count == 0) {
      break;
    }
    et::TerminalBuffer tb;
    tb.set_buffer(string(data, count));
    conn->writePacket(
        Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
    buf->consume(count);
  }
  return false;
}

void drainDiscardJumphostTerminalBuffers(
    shared_ptr<SocketHandler> terminalSocketHandler, int terminalFd,
    JumphostPending* pending) {
  bool addedTerminal = false;
  while (terminalSocketHandler->hasData(terminalFd)) {
    Packet packet;
    if (!terminalSocketHandler->readPacket(terminalFd, &packet)) {
      continue;
    }
    if (packet.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
      addedTerminal = true;
    }
    pending->enqueue(packet);
  }
  if (addedTerminal) {
    pending->filterTerminalBuffers();
  }
}

}  // namespace

TerminalServer::TerminalServer(
    std::shared_ptr<SocketHandler> _socketHandler,
    const SocketEndpoint& _serverEndpoint,
    std::shared_ptr<PipeSocketHandler> _pipeSocketHandler,
    const SocketEndpoint& _routerEndpoint)
    : ServerConnection(_socketHandler, _serverEndpoint),
      routerEndpoint(_routerEndpoint) {
  terminalRouter = shared_ptr<UserTerminalRouter>(
      new UserTerminalRouter(_pipeSocketHandler, _routerEndpoint));
}

TerminalServer::~TerminalServer() {}

void TerminalServer::run() {
  LOG(INFO) << "Creating server";
  set<int> serverPortFds = socketHandler->getEndpointFds(serverEndpoint);
  set<int> coreFds = serverPortFds;
  coreFds.insert(terminalRouter->getServerFd());
  FdPoller poller;
  poller.setFds(coreFds);

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
    set<int> readyFds = poller.wait(100).readable;
    if (readyFds.empty()) {
      continue;
    }

    for (int i : serverPortFds) {
      if (readyFds.count(i) != 0) {
        acceptNewConnection(i);
      }
    }
    if (readyFds.count(terminalRouter->getServerFd()) != 0) {
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
    const InitialPayload& payload, const TerminalUserInfo& userInfo) {
  InitialResponse response;
  serverClientState->writePacket(
      Packet(uint8_t(EtPacketType::INITIAL_RESPONSE), protoToString(response)));
  // set thread name
  el::Helpers::setThreadName(serverClientState->getId());
  bool run = true;

  int terminalFd = userInfo.fd();
  shared_ptr<SocketHandler> terminalSocketHandler =
      terminalRouter->getSocketHandler();
  FdPoller poller;

  terminalSocketHandler->writePacket(
      terminalFd,
      Packet(TerminalPacketType::JUMPHOST_INIT, protoToString(payload)));

  JumphostPending pending;
  string jumphostInterruptCarry;
  bool holdDroppableForClient = false;

  while (true) {
    {
      lock_guard<std::mutex> guard(terminalThreadMutex);
      if (halt || !run || serverClientState->isShuttingDown()) {
        break;
      }
    }

    set<int> readFds;
    set<int> writeFds;
    set<int> refreshFds;
    int serverClientFd = serverClientState->getSocketFd();
    const bool connected = serverClientFd > 0;
    // Connected: bound the userspace queue. Disconnected: keep today's
    // 64MB BackedWriter path so long-running jobs do not stall.
    bool readTerminal = connected
                            ? pending.canAcceptMore()
                            : serverClientState->canBufferWrite(2 * BUF_SIZE);
    if (readTerminal && !holdDroppableForClient) {
      readFds.insert(terminalFd);
    }
    if (connected) {
      getSocketHandler()->minimizeKernelBuffering(serverClientFd);
      readFds.insert(serverClientFd);
      // A reconnect can hand back the same fd number for a new socket.
      refreshFds.insert(serverClientFd);
      if (!pending.empty() && !holdDroppableForClient) {
        writeFds.insert(serverClientFd);
      }
    }
    poller.setFds(readFds, writeFds, refreshFds);
    // Write readiness only needs to wake the loop; the drain below re-checks
    // it per write.
    const set<int> readyFds = poller.wait(100).readable;

    try {
      // Read client input before draining, so Ctrl+C can drop the backlog
      // instead of losing the race to a writable socket.
      if (serverClientFd > 0 && readyFds.count(serverClientFd) != 0) {
        VLOG(4) << "Jumphost socket is ready";
        if (serverClientState->hasData()) {
          VLOG(4) << "Jumphost serverClientState has data";
          Packet packet;
          if (serverClientState->readPacket(&packet)) {
            if (packet.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
              et::TerminalBuffer tb =
                  stringToProto<et::TerminalBuffer>(packet.getPayload());
              if (WriteBuffer::containsInterruptByte(tb.buffer()) ||
                  tmuxCcInputRequestsInterrupt(jumphostInterruptCarry,
                                               tb.buffer())) {
                size_t dropped = pending.flushTerminalBuffersIfLarge();
                if (dropped > 0) {
                  LOG(INFO)
                      << "Flushed " << dropped
                      << " bytes of jumphost terminal output on interrupt";
                  drainDiscardJumphostTerminalBuffers(terminalSocketHandler,
                                                      terminalFd, &pending);
                } else {
                  LOG(INFO) << "Jumphost interrupt with only " << pending.size()
                            << " buffered bytes";
                }
              }
              tmuxCcRetainIncompleteLine(&jumphostInterruptCarry, tb.buffer());
            }
            try {
              terminalSocketHandler->writePacket(terminalFd, packet);
              VLOG(4) << "Jumphost wrote to router " << terminalFd;
            } catch (const std::runtime_error& ex) {
              LOG(INFO)
                  << "Unix socket died between global daemon and terminal "
                     "router: "
                  << ex.what();
              run = false;
              break;
            }
          }
        }
      }

      serverClientFd = serverClientState->getSocketFd();
      const bool stillConnected = serverClientFd > 0;
      if (holdDroppableForClient) {
        holdDroppableForClient = false;
      } else {
        holdDroppableForClient = pending.drainToClient(
            serverClientState.get(), stillConnected ? serverClientFd : -1);
      }

      if (readyFds.count(terminalFd) != 0) {
        try {
          Packet packet;
          if (terminalSocketHandler->readPacket(terminalFd, &packet)) {
            if (stillConnected) {
              pending.enqueue(packet);
              if (!holdDroppableForClient) {
                holdDroppableForClient = pending.drainToClient(
                    serverClientState.get(), serverClientFd);
              }
            } else {
              serverClientState->writePacket(packet);
            }
          }
        } catch (const std::runtime_error& ex) {
          LOG(INFO) << "Terminal session ended" << ex.what();
          run = false;
          break;
        }
      }
    } catch (const runtime_error& re) {
      STERROR << "Jumphost Error: " << re.what();
      CLOG(INFO, "stdout") << "ERROR: " << re.what();
      serverClientState->closeSocket();
    }
  }
}

void TerminalServer::runTerminal(
    shared_ptr<ServerClientConnection> serverClientState,
    const InitialPayload& payload, const TerminalUserInfo& userInfo) {
  InitialResponse response;
  shared_ptr<SocketHandler> serverSocketHandler = getSocketHandler();
  shared_ptr<SocketHandler> pipeSocketHandler(new PipeSocketHandler());
  shared_ptr<PortForwardHandler> portForwardHandler(new PortForwardHandler(
      serverSocketHandler, pipeSocketHandler, userInfo.uid(), userInfo.gid()));
  map<string, string> environmentVariables;

  for (const auto& envVar : payload.environmentvariables()) {
    environmentVariables[envVar.first] = envVar.second;
    LOG(INFO) << "SetEnv: " << envVar.first << "=" << envVar.second;
  }

  vector<string> pipePaths;
  for (const PortForwardSourceRequest& pfsr : payload.reversetunnels()) {
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
  FdPoller poller;
  uint64_t forwardFdsGeneration = portForwardHandler->getForwardFdsGeneration();

  TermInit termInit;
  for (auto& it : environmentVariables) {
    *(termInit.add_environmentnames()) = it.first;
    *(termInit.add_environmentvalues()) = it.second;
  }
  terminalSocketHandler->writePacket(
      terminalFd,
      Packet(TerminalPacketType::TERMINAL_INIT, protoToString(termInit)));

  WriteBuffer terminalOutputBuffer;
  string clientInterruptCarry;
  bool holdDroppableForClient = false;

  while (run) {
    {
      lock_guard<std::mutex> guard(terminalThreadMutex);
      if (halt || serverClientState->isShuttingDown()) {
        break;
      }
    }

    set<int> readFds;
    set<int> writeFds;
    set<int> refreshFds;
    int serverClientFd = serverClientState->getSocketFd();
    const bool connected = serverClientFd > 0;
    // Connected: stage in WriteBuffer (16MB cap). Disconnected: today's
    // 64MB BackedWriter path, so a job keeps running after the laptop
    // closes.
    bool readTerminal = connected
                            ? terminalOutputBuffer.canAcceptMore()
                            : serverClientState->canBufferWrite(2 * BUF_SIZE);
    if (readTerminal && !holdDroppableForClient) {
      readFds.insert(terminalFd);
    }
    if (connected) {
      // Reapply every iteration: reconnect replaces the socket, kernel
      // tuning is per-socket, and fd numbers are reused.
      serverSocketHandler->minimizeKernelBuffering(serverClientFd);
      readFds.insert(serverClientFd);
      refreshFds.insert(serverClientFd);
      if (terminalOutputBuffer.hasPendingData() && !holdDroppableForClient) {
        writeFds.insert(serverClientFd);
      }
    }
    // Include port forward sockets for low-latency forwarding.
    set<int> pfFds;
    portForwardHandler->getForwardFds(&pfFds);
    readFds.insert(pfFds.begin(), pfFds.end());

    // Any open or close since the last pass may have recycled an fd number.
    uint64_t currentForwardFdsGeneration =
        portForwardHandler->getForwardFdsGeneration();
    if (currentForwardFdsGeneration != forwardFdsGeneration) {
      refreshFds.insert(pfFds.begin(), pfFds.end());
      forwardFdsGeneration = currentForwardFdsGeneration;
    }
    poller.setFds(readFds, writeFds, refreshFds);
    // Write readiness only needs to wake the loop; the drain below re-checks
    // it per write.
    const set<int> readyFds = poller.wait(100).readable;

    try {
      // Handle client input before draining the output queue. Otherwise a
      // writable socket (fast client, or a client just resumed) sends the
      // whole backlog before Ctrl+C is read, and flushIfLarge sees nothing.
      if (serverClientFd > 0 && readyFds.count(serverClientFd) != 0) {
        VLOG(3) << "ServerClientFd is ready";
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
              if (WriteBuffer::containsInterruptByte(tb.buffer()) ||
                  tmuxCcInputRequestsInterrupt(clientInterruptCarry,
                                               tb.buffer())) {
                LOG(INFO) << "Interrupt from client (" << tb.buffer().size()
                          << " bytes), WriteBuffer="
                          << terminalOutputBuffer.size();
                size_t dropped = terminalOutputBuffer.flushIfLarge();
                if (dropped > 0) {
                  LOG(INFO) << "Flushed " << dropped
                            << " bytes of terminal output on interrupt";
                  drainDiscardReadableBytes(terminalFd, &terminalOutputBuffer);
                }
              }
              tmuxCcRetainIncompleteLine(&clientInterruptCarry, tb.buffer());
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

      // The client may have disconnected while we were reading its packets.
      // Re-check so leftover WriteBuffer goes to BackedWriter (64MB) instead
      // of staying gated on the connected 16MB cap.
      serverClientFd = serverClientState->getSocketFd();
      const bool stillConnected = serverClientFd > 0;
      if (holdDroppableForClient) {
        // Already waited one select for send-keys. Resume droppable
        // drain only after that wait; do not write flood in this gap.
        holdDroppableForClient = false;
      } else {
        holdDroppableForClient =
            drainWriteBufferToClient(&terminalOutputBuffer, serverClientState,
                                     stillConnected ? serverClientFd : -1);
      }

      // Check for data to receive; the received
      // data includes also the data previously sent
      // on the same master descriptor (line 90).
      if (readyFds.count(terminalFd) != 0) {
        // Read from terminal and write to client
        memset(b, 0, BUF_SIZE);
        int rc = read(terminalFd, b, BUF_SIZE);
        if (rc > 0) {
          VLOG(2) << "Sending bytes from terminal: " << rc << " "
                  << serverClientState->getWriter()->getSequenceNumber();
          string s(b, rc);
          if (stillConnected) {
            terminalOutputBuffer.enqueue(s);
            if (!holdDroppableForClient) {
              holdDroppableForClient = drainWriteBufferToClient(
                  &terminalOutputBuffer, serverClientState, serverClientFd);
            }
          } else {
            et::TerminalBuffer tb;
            tb.set_buffer(s);
            serverClientState->writePacket(
                Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
          }
        } else if (rc == 0) {
          LOG(INFO) << "Terminal session ended";
          run = false;
          break;
        } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
          // Common after a Ctrl+C drain-discard of already-readable bytes.
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
      portForwardHandler->update(&requests, &dataToSend, &readyFds);
      for (auto& pfr : requests) {
        serverClientState->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST,
                   protoToString(pfr)));
      }
      for (auto& pwd : dataToSend) {
        serverClientState->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DATA, protoToString(pwd)));
      }
    } catch (const runtime_error& re) {
      STERROR << "Error: " << re.what();
      CLOG(INFO, "stdout") << "Error: " << re.what();
      serverClientState->closeSocket();
      // recoverClient() resumes this session after a transient disconnect.
    }
  }
}

void TerminalServer::handleConnection(
    shared_ptr<ServerClientConnection> serverClientState) {
  std::optional<TerminalUserInfo> userInfo;
  try {
    Packet packet;
    const time_t initialPayloadDeadline = time(NULL) + initialPayloadTimeoutSec;
    while (!serverClientState->readPacket(&packet)) {
      bool halted;
      {
        lock_guard<std::mutex> guard(terminalThreadMutex);
        halted = halt;
      }
      // Every exit matters: without them this thread spins forever on a client
      // that never speaks, and run() blocks on join() at shutdown.
      if (halted || serverClientState->isShuttingDown() ||
          time(NULL) > initialPayloadDeadline) {
        LOG(WARNING) << "Giving up waiting for the initial packet from "
                     << serverClientState->getId();
        removeClient(serverClientState->getId());
        return;
      }
      LOG_EVERY_N(10, INFO) << "Waiting for initial packet...";
      sleep(1);
    }
    if (packet.getHeader() != EtPacketType::INITIAL_PAYLOAD) {
      STFATAL << "Invalid header: expecting INITIAL_PAYLOAD but got "
              << packet.getHeader();
    }
    InitialPayload payload = stringToProto<InitialPayload>(packet.getPayload());
    userInfo = terminalRouter->tryGetInfoForConnection(serverClientState);
    if (!userInfo) {
      LOG(ERROR) << "Client failed to bind to terminal router";
    } else if (payload.jumphost()) {
      LOG(INFO) << "RUNNING JUMPHOST";
      runJumpHost(serverClientState, payload, *userInfo);
    } else {
      LOG(INFO) << "RUNNING TERMINAL";
      runTerminal(serverClientState, payload, *userInfo);
    }
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Terminal thread failed: " << ex.what();
  }

  removeClient(serverClientState->getId());
  if (userInfo) {
    terminalRouter->removeConnection(*userInfo);
  }
}

bool TerminalServer::newClient(
    shared_ptr<ServerClientConnection> serverClientState) {
  lock_guard<std::mutex> guard(terminalThreadMutex);
  auto t = make_shared<thread>(&TerminalServer::handleConnection, this,
                               serverClientState);
  terminalThreads.push_back(t);
  return true;
}
}  // namespace et
#endif
