// Make struct ucred / SO_PEERCRED visible on glibc (Linux peer-credential check).
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#ifndef WIN32
#include "ControlListener.hpp"

#include "ControlProtocol.hpp"
#include "ETerminal.pb.h"

namespace et {

ControlListener::ControlListener(shared_ptr<ControlConsole> _console,
                                 const string& _socketPath,
                                 std::function<void()> _onKill,
                                 std::function<bool()> _isConnected,
                                 const string& _host)
    : console(_console),
      socketPath(_socketPath),
      onKill(_onKill),
      isConnected(_isConnected),
      host(_host),
      listenFd(-1),
      running(false) {}

ControlListener::~ControlListener() { shutdown(); }

void ControlListener::start() {
  listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(listenFd);

  // A stale socket from a crashed daemon would block bind(); remove it first.
  ::unlink(socketPath.c_str());

  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (socketPath.size() >= sizeof(addr.sun_path)) {
    throw std::runtime_error("control socket path too long: " + socketPath);
  }
  strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

  // Create the socket with restrictive perms regardless of umask.
  const mode_t oldMask = ::umask(0077);
  int rc = ::bind(listenFd, (sockaddr*)&addr, sizeof(addr));
  ::umask(oldMask);
  if (rc < 0) {
    ::close(listenFd);
    listenFd = -1;
    throw std::runtime_error("could not bind control socket " + socketPath +
                             ": " + strerror(errno));
  }
  FATAL_FAIL(::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR));
  FATAL_FAIL(::listen(listenFd, 8));

  running = true;
  acceptThread = std::thread(&ControlListener::acceptLoop, this);
}

void ControlListener::shutdown() {
  bool wasRunning = running.exchange(false);
  if (listenFd >= 0) {
    ::shutdown(listenFd, SHUT_RDWR);
    ::close(listenFd);
    listenFd = -1;
  }
  if (acceptThread.joinable()) {
    acceptThread.join();
  }
  if (wasRunning) {
    ::unlink(socketPath.c_str());
  }
}

void ControlListener::acceptLoop() {
  el::Helpers::setThreadName("control-listener");
  while (running) {
    int connFd = ::accept(listenFd, NULL, NULL);
    if (connFd < 0) {
      if (!running) {
        break;
      }
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      break;  // listen socket closed during shutdown
    }
    try {
      handleConnection(connFd);
    } catch (const std::exception& e) {
      LOG(INFO) << "Control connection error: " << e.what();
    }
    ::close(connFd);
  }
}

bool ControlListener::peerAuthorized(int connFd) const {
  // Only the owning user may drive the session, even if perms were loosened.
  uid_t peerUid = (uid_t)-1;
#ifdef __linux__
  // Linux reports peer credentials through SO_PEERCRED (getpeereid is BSD-only).
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (::getsockopt(connFd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    return false;
  }
  peerUid = cred.uid;
#else
  // BSD / macOS: getpeereid reports the connected peer's effective uid.
  gid_t peerGid = (gid_t)-1;
  if (::getpeereid(connFd, &peerUid, &peerGid) != 0) {
    return false;
  }
  (void)peerGid;
#endif
  return peerUid == ::geteuid();
}

void ControlListener::handleConnection(int connFd) {
  if (!peerAuthorized(connFd)) {
    control_proto::writeFrame(connFd, CTL_ERR, "permission denied");
    return;
  }

  uint8_t opcode = 0;
  string payload;
  if (!control_proto::readFrame(connFd, &opcode, &payload)) {
    return;  // client closed without a request
  }

  switch (opcode) {
    case CTL_WRITE: {
      console->injectInput(payload);
      control_proto::writeFrame(connFd, CTL_OK, "");
      break;
    }
    case CTL_WRITE_SECRET: {
      console->injectInput(payload, /*secret=*/true);
      control_proto::writeFrame(connFd, CTL_OK, "");
      break;
    }
    case CTL_RESIZE: {
      TerminalInfo ti;
      if (!ti.ParseFromString(payload)) {
        control_proto::writeFrame(connFd, CTL_ERR, "bad TerminalInfo");
        break;
      }
      console->setSize(ti.row(), ti.column(), ti.width(), ti.height());
      control_proto::writeFrame(connFd, CTL_OK, "");
      break;
    }
    case CTL_READ: {
      int64_t cursor = control_proto::decodeCursor(payload);
      ScrollbackRead r = console->readOutput(cursor);
      control_proto::writeFrame(connFd, CTL_READ_RESP,
                                control_proto::encodeReadResp(r));
      break;
    }
    case CTL_SNIFF: {
      int64_t cursor = control_proto::decodeCursor(payload);
      TranscriptRead tr = console->readTranscript(cursor);
      control_proto::writeFrame(connFd, CTL_SNIFF_RESP,
                                control_proto::encodeTranscriptResp(tr));
      break;
    }
    case CTL_INFO: {
      TerminalInfo ti = console->getTerminalInfo();
      const bool connected = isConnected ? isConnected() : true;
      std::ostringstream out;
      out << "alive=1\n"
          << "connected=" << (connected ? 1 : 0) << "\n"
          << "host=" << host << "\n"
          << "pid=" << (long long)::getpid() << "\n"
          << "rows=" << ti.row() << "\n"
          << "cols=" << ti.column() << "\n"
          << "headCursor=" << (long long)console->headCursor() << "\n"
          << "created=" << (long long)console->createdAt() << "\n"
          << "lastActivity=" << (long long)console->lastActivity() << "\n";
      control_proto::writeFrame(connFd, CTL_INFO_RESP, out.str());
      break;
    }
    case CTL_KILL: {
      control_proto::writeFrame(connFd, CTL_OK, "");
      if (onKill) {
        onKill();
      }
      break;
    }
    default:
      control_proto::writeFrame(connFd, CTL_ERR, "unknown opcode");
      break;
  }
}

}  // namespace et
#endif
