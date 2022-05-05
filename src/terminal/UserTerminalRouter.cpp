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
  lock_guard<recursive_mutex> guard(routerMutex);
  LOG(INFO) << "Listening to id/key FIFO";
  const int terminalFd = socketHandler->accept(serverFd);
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

    const bool inserted =
        idInfoMap.insert(std::make_pair(tui.id(), tui)).second;
    if (!inserted) {
      LOG(ERROR) << "Rejecting duplicate terminal connection for " << tui.id();
      socketHandler->close(terminalFd);
      return IdKeyPair({"", ""});
    }

    return IdKeyPair({tui.id(), tui.passkey()});
  } catch (const std::runtime_error &re) {
    LOG(ERROR) << "Router can't talk to terminal: " << re.what();
    socketHandler->close(terminalFd);
    return IdKeyPair({"", ""});
  }

  STFATAL << "Should never get here";
  return IdKeyPair({"", ""});
}

std::optional<TerminalUserInfo> UserTerminalRouter::tryGetInfoForConnection(
    const shared_ptr<ServerClientConnection> &serverClientState) {
  lock_guard<recursive_mutex> guard(routerMutex);
  auto it = idInfoMap.find(serverClientState->getId());
  if (it == idInfoMap.end()) {
    STFATAL << " Tried to read from an id that no longer exists";
  }

  // While both the id and passkey are randomly generated, do an extra
  // verification that the passkey matches to ensure that this is the intended
  // serverClientState.
  if (!serverClientState->verifyPasskey(it->second.passkey())) {
    LOG(ERROR) << "Failed to verify passkey for client id: " << it->second.id();
    return std::nullopt;
  }

  return it->second;
}

}  // namespace et
#endif
