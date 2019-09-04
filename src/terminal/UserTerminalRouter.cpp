#include "UserTerminalRouter.hpp"

#include "ETerminal.pb.h"

namespace et {
UserTerminalRouter::UserTerminalRouter(
    shared_ptr<PipeSocketHandler> _socketHandler,
    const SocketEndpoint &_routerEndpoint)
    : socketHandler(_socketHandler) {
  serverFd = *(socketHandler->listen(_routerEndpoint).begin());
  FATAL_FAIL(::chown(_routerEndpoint.name().c_str(), getuid(), getgid()));
  FATAL_FAIL(::chmod(_routerEndpoint.name().c_str(),
                     S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP |
                         S_IROTH | S_IWOTH | S_IXOTH));
}

IdKeyPair UserTerminalRouter::acceptNewConnection() {
  LOG(INFO) << "Listening to id/key FIFO";
  int terminalFd = socketHandler->accept(serverFd);
  if (terminalFd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      FATAL_FAIL(-1);  // LOG(FATAL) with the error
    } else {
      return IdKeyPair({"", ""});  // Nothing to accept this time
    }
  }

  LOG(INFO) << "Connected";

  try {
    Packet packet = socketHandler->readPacket(terminalFd);
    if (packet.getHeader() != TerminalPacketType::TERMINAL_USER_INFO) {
      LOG(FATAL) << "Got an invalid packet header: " << int(packet.getHeader());
    }
    TerminalUserInfo tui = stringToProto<TerminalUserInfo>(packet.getPayload());
    VLOG(1) << "Got id/passkey: " << tui.id() << "/" << tui.passkey();
    tui.set_fd(terminalFd);
    idInfoMap[tui.id()] = tui;
    return IdKeyPair({tui.id(), tui.passkey()});
  } catch (const std::runtime_error &re) {
    LOG(FATAL) << "Router can't talk to terminal: " << re.what();
  }

  return IdKeyPair({"", ""});
}

TerminalUserInfo UserTerminalRouter::getInfoForId(const string &id) {
  auto it = idInfoMap.find(id);
  if (it == idInfoMap.end()) {
    LOG(FATAL) << " Tried to read from an id that no longer exists";
  }
  return it->second;
}
}  // namespace et
