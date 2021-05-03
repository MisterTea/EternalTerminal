#ifndef WIN32
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
    if (GetErrno() != EAGAIN && GetErrno() != EWOULDBLOCK) {
      FATAL_FAIL(-1);  // STFATAL with the error
    } else {
      return IdKeyPair({"", ""});  // Nothing to accept this time
    }
  }

  LOG(INFO) << "Connected";

  try {
    Packet packet;
    if (!socketHandler->readPacket(terminalFd, &packet)) {
      STFATAL << "Missing user info packet";
    }
    if (packet.getHeader() != TerminalPacketType::TERMINAL_USER_INFO) {
      STFATAL << "Got an invalid packet header: " << int(packet.getHeader());
    }
    TerminalUserInfo tui = stringToProto<TerminalUserInfo>(packet.getPayload());
    tui.set_fd(terminalFd);
    idInfoMap[tui.id()] = tui;
    return IdKeyPair({tui.id(), tui.passkey()});
  } catch (const std::runtime_error &re) {
    STFATAL << "Router can't talk to terminal: " << re.what();
  }

  STFATAL << "Should never get here";
  return IdKeyPair({"", ""});
}

TerminalUserInfo UserTerminalRouter::getInfoForId(const string &id) {
  auto it = idInfoMap.find(id);
  if (it == idInfoMap.end()) {
    STFATAL << " Tried to read from an id that no longer exists";
  }
  return it->second;
}
}  // namespace et
#endif
