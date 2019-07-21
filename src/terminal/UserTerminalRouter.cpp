#include "UserTerminalRouter.hpp"

namespace et {
UserTerminalRouter::UserTerminalRouter(
    shared_ptr<PipeSocketHandler> _socketHandler, const string &routerFifoName)
    : socketHandler(_socketHandler) {
  serverFd = *(socketHandler->listen(SocketEndpoint(routerFifoName)).begin());
}

optional<IdKeyPair> UserTerminalRouter::acceptNewConnection() {
  LOG(INFO) << "Listening to id/key FIFO";
  int terminalFd = socketHandler->accept(serverFd);
  if (terminalFd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      FATAL_FAIL(-1);  // LOG(FATAL) with the error
    } else {
      return nullopt;  // Nothing to accept this time
    }
  }

  LOG(INFO) << "Connected";

  try {
    Packet packet = socketHandler->readPacket(terminalFd);
    if (packet.getHeader() != TerminalPacketType::IDPASSKEY) {
      LOG(FATAL) << "Got an invalid packet header: " << packet.getHeader();
    }
    string buf = packet.getPayload();
    VLOG(1) << "Got passkey: " << buf;
    size_t slashIndex = buf.find("/");
    if (slashIndex == string::npos) {
      LOG(ERROR) << "Invalid idPasskey id/key pair: " << buf;
      close(terminalFd);
    } else {
      string id = buf.substr(0, slashIndex);
      string key = buf.substr(slashIndex + 1);
      idFdMap[id] = terminalFd;
      return IdKeyPair({id, key});
    }
  } catch (const std::runtime_error &re) {
    LOG(FATAL) << "Router can't talk to terminal: " << re.what();
  }

  return nullopt;
}

int UserTerminalRouter::getFd(const string &id) {
  auto it = idFdMap.find(id);
  if (it == idFdMap.end()) {
    LOG(FATAL) << " Tried to read from an id that no longer exists";
  }
  return it->second;
}
}  // namespace et
