#include "MuxClient.hpp"

#ifndef WIN32
#include <sys/socket.h>
#include <sys/un.h>
#else
// clang-format off
#include <winsock2.h>
#include <afunix.h>
// clang-format on
#endif

namespace et {

MuxClient::MuxClient(string controlPath) : path(std::move(controlPath)) {}

MuxClient::~MuxClient() { disconnect(); }

void MuxClient::disconnect() { conn.reset(); }

bool MuxClient::connect(int timeoutMs) {
  disconnect();
  if (path.empty()) {
    return false;
  }

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return false;
  }
  memcpy(addr.sun_path, path.c_str(), path.size() + 1);
#ifndef WIN32
  socklen_t addrLen =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
#else
  socklen_t addrLen = sizeof(addr);
#endif

  // Non-blocking connect with optional timeout.
#ifndef WIN32
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
#endif
  int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), addrLen);
  if (rc < 0) {
    if (errno != EINPROGRESS && errno != EAGAIN
#ifdef WIN32
        && errno != WSAEWOULDBLOCK && errno != WSAEINPROGRESS
#endif
    ) {
      ::close(fd);
      return false;
    }
    int prc = muxPollFd(fd, POLLOUT, timeoutMs);
    if (prc <= 0) {
      ::close(fd);
      return false;
    }
    int soError = 0;
    socklen_t len = sizeof(soError);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&soError), &len) < 0 ||
        soError != 0) {
      ::close(fd);
      return false;
    }
  }
#ifndef WIN32
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags);
  }
#endif

  conn = make_unique<MuxConnection>(fd, true);
  if (!exchangeHello()) {
    disconnect();
    return false;
  }
  return true;
}

bool MuxClient::exchangeHello() {
  // Match OpenSSH: client sends HELLO first, then reads the master's HELLO.
  MuxBuffer hello;
  hello.putU32(MUX_MSG_HELLO);
  hello.putU32(SSHMUX_VER);
  if (!conn->writePacket(hello)) {
    return false;
  }

  MuxBuffer incoming;
  if (!conn->readPacket(&incoming, 5000)) {
    return false;
  }
  uint32_t type = 0;
  uint32_t ver = 0;
  if (!incoming.getU32(&type) || type != MUX_MSG_HELLO ||
      !incoming.getU32(&ver) || ver != SSHMUX_VER) {
    return false;
  }
  while (incoming.remaining() > 0) {
    string name;
    string value;
    if (!incoming.getString(&name) || !incoming.getString(&value)) {
      return false;
    }
  }
  return true;
}

bool MuxClient::expectReply(uint32_t expectedRequestId, uint32_t* typeOut,
                            MuxBuffer* body) {
  if (!conn->readPacket(body, 5000)) {
    return false;
  }
  uint32_t type = 0;
  uint32_t rid = 0;
  if (!body->getU32(&type) || !body->getU32(&rid)) {
    return false;
  }
  if (rid != expectedRequestId) {
    return false;
  }
  *typeOut = type;
  return true;
}

bool MuxClient::aliveCheck(uint32_t* masterPid) {
  if (!conn) {
    return false;
  }
  uint32_t rid = nextRequestId++;
  MuxBuffer req;
  req.putU32(MUX_C_ALIVE_CHECK);
  req.putU32(rid);
  if (!conn->writePacket(req)) {
    return false;
  }
  MuxBuffer body;
  uint32_t type = 0;
  if (!expectReply(rid, &type, &body) || type != MUX_S_ALIVE) {
    return false;
  }
  uint32_t pid = 0;
  if (!body.getU32(&pid)) {
    return false;
  }
  if (masterPid) {
    *masterPid = pid;
  }
  return true;
}

bool MuxClient::terminateMaster() {
  if (!conn) {
    return false;
  }
  uint32_t rid = nextRequestId++;
  MuxBuffer req;
  req.putU32(MUX_C_TERMINATE);
  req.putU32(rid);
  if (!conn->writePacket(req)) {
    return false;
  }
  MuxBuffer body;
  uint32_t type = 0;
  if (!expectReply(rid, &type, &body)) {
    return false;
  }
  return type == MUX_S_OK;
}

bool MuxClient::stopListening() {
  if (!conn) {
    return false;
  }
  uint32_t rid = nextRequestId++;
  MuxBuffer req;
  req.putU32(MUX_C_STOP_LISTENING);
  req.putU32(rid);
  if (!conn->writePacket(req)) {
    return false;
  }
  MuxBuffer body;
  uint32_t type = 0;
  if (!expectReply(rid, &type, &body)) {
    return false;
  }
  return type == MUX_S_OK;
}

bool MuxClient::openForward(const MuxOpenForwardRequest& fwd, string* error) {
  if (!conn) {
    return false;
  }
  uint32_t rid = nextRequestId++;
  MuxBuffer req;
  req.putU32(MUX_C_OPEN_FWD);
  req.putU32(rid);
  req.putU32(fwd.type);
  req.putString(fwd.listenHost);
  req.putU32(fwd.listenPort);
  req.putString(fwd.connectHost);
  req.putU32(fwd.connectPort);
  if (!conn->writePacket(req)) {
    return false;
  }
  MuxBuffer body;
  uint32_t type = 0;
  if (!expectReply(rid, &type, &body)) {
    return false;
  }
  if (type == MUX_S_OK || type == MUX_S_REMOTE_PORT) {
    return true;
  }
  string reason;
  body.getString(&reason);
  if (error) {
    *error = reason;
  }
  return false;
}

bool MuxClient::closeForward(const MuxOpenForwardRequest& fwd, string* error) {
  if (!conn) {
    return false;
  }
  uint32_t rid = nextRequestId++;
  MuxBuffer req;
  req.putU32(MUX_C_CLOSE_FWD);
  req.putU32(rid);
  req.putU32(fwd.type);
  req.putString(fwd.listenHost);
  req.putU32(fwd.listenPort);
  req.putString(fwd.connectHost);
  req.putU32(fwd.connectPort);
  if (!conn->writePacket(req)) {
    return false;
  }
  MuxBuffer body;
  uint32_t type = 0;
  if (!expectReply(rid, &type, &body)) {
    return false;
  }
  if (type == MUX_S_OK) {
    return true;
  }
  string reason;
  body.getString(&reason);
  if (error) {
    *error = reason;
  }
  return false;
}

bool MuxClient::newSession(const string& command, bool wantTty, int stdinFd,
                           int stdoutFd, int stderrFd, uint32_t* sessionId,
                           string* error) {
  if (!conn) {
    return false;
  }
  uint32_t rid = nextRequestId++;
  MuxBuffer req;
  req.putU32(MUX_C_NEW_SESSION);
  req.putU32(rid);
  req.putString("");  // reserved
  req.putBool(wantTty);
  req.putBool(false);  // x11
  req.putBool(false);  // agent
  req.putBool(false);  // subsystem
  req.putU32(0xffffffffu);
  const char* termEnv = getenv("TERM");
  req.putString(termEnv ? termEnv : "");
  req.putString(command);
  if (!conn->writePacket(req)) {
    return false;
  }

  int localNull = -1;
  auto ensureFd = [&](int fd) {
    if (fd >= 0) {
      return fd;
    }
#ifndef WIN32
    if (localNull < 0) {
      localNull = ::open("/dev/null", O_RDWR);
    }
    return localNull;
#else
    return fd;
#endif
  };
  int inFd = ensureFd(stdinFd);
  int outFd = ensureFd(stdoutFd);
  int errFd = ensureFd(stderrFd);
  if (inFd < 0 || outFd < 0 || errFd < 0 || !conn->sendFd(inFd) ||
      !conn->sendFd(outFd) || !conn->sendFd(errFd)) {
    if (localNull >= 0) {
      ::close(localNull);
    }
    if (error) {
      *error = "failed to send stdio fds";
    }
    return false;
  }
  if (localNull >= 0) {
    ::close(localNull);
  }

  MuxBuffer body;
  uint32_t type = 0;
  if (!expectReply(rid, &type, &body)) {
    return false;
  }
  if (type != MUX_S_SESSION_OPENED) {
    string reason;
    body.getString(&reason);
    if (error) {
      *error = reason.empty() ? "new session failed" : reason;
    }
    return false;
  }
  uint32_t sid = 0;
  if (!body.getU32(&sid)) {
    return false;
  }
  if (sessionId) {
    *sessionId = sid;
  }

  // Master may follow with EXIT_MESSAGE for short shared attaches.
  MuxBuffer exitBody;
  if (conn->readPacket(&exitBody, 2000)) {
    uint32_t exitType = 0;
    exitBody.getU32(&exitType);
  }
  return true;
}

int MuxClient::runCtlCommand(const string& command) {
  if (!connect()) {
    CLOG(INFO, "stdout") << "Control socket connect failed: " << path << endl;
    return 1;
  }
  string cmd = command;
  for (char& c : cmd) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  if (cmd == "check") {
    uint32_t pid = 0;
    if (!aliveCheck(&pid)) {
      CLOG(INFO, "stdout") << "Master alive check failed" << endl;
      return 1;
    }
    CLOG(INFO, "stdout") << "Master running (pid=" << pid << ")" << endl;
    return 0;
  }
  if (cmd == "exit") {
    if (!terminateMaster()) {
      CLOG(INFO, "stdout") << "Failed to terminate master" << endl;
      return 1;
    }
    return 0;
  }
  if (cmd == "stop") {
    if (!stopListening()) {
      CLOG(INFO, "stdout") << "Failed to stop master listener" << endl;
      return 1;
    }
    return 0;
  }
  if (cmd == "forward" || cmd == "cancel") {
    // Forward/cancel require tunnel specs on the command line; handled by
    // TerminalClientMain which calls openForward/closeForward directly.
    CLOG(INFO, "stdout") << "-O " << cmd
                         << " requires tunnel options (-t) on this invocation"
                         << endl;
    return 1;
  }
  CLOG(INFO, "stdout") << "Unsupported -O command: " << command << endl;
  return 1;
}

bool shouldAttachToMuxMaster(const MuxOptions& opts) {
  if (opts.controlPath.empty()) {
    return false;
  }
  if (!opts.ctlCommand.empty()) {
    return true;
  }
  if (opts.controlMaster == ControlMasterMode::Yes) {
    return false;
  }
  return controlPathSocketExists(opts.controlPath);
}

bool shouldBecomeMuxMaster(const MuxOptions& opts) {
  if (opts.controlPath.empty()) {
    return false;
  }
  if (!opts.ctlCommand.empty()) {
    return false;
  }
  if (opts.controlMaster == ControlMasterMode::Yes) {
    return true;
  }
  if (opts.controlMaster == ControlMasterMode::Auto) {
    return !controlPathSocketExists(opts.controlPath);
  }
  return false;
}

}  // namespace et
