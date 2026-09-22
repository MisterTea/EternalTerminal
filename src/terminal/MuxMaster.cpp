#include "MuxMaster.hpp"

#include "PipeSocketHandler.hpp"
#include "TunnelUtils.hpp"

#ifdef WIN32
#include <afunix.h>
#endif

namespace et {
namespace {

bool forwardsEqual(const MuxTrackedForward& a, const MuxTrackedForward& b) {
  return a.type == b.type && a.listenHost == b.listenHost &&
         a.listenPort == b.listenPort && a.connectHost == b.connectHost &&
         a.connectPort == b.connectPort;
}

}  // namespace

MuxMaster::MuxMaster(string controlPath, ControlPersistConfig persist)
    : path(std::move(controlPath)), persistConfig(persist) {}

MuxMaster::~MuxMaster() { stop(); }

void MuxMaster::setPortForwardHandler(shared_ptr<PortForwardHandler> handler) {
  lock_guard<recursive_mutex> guard(mutex);
  portForwardHandler = std::move(handler);
}

string MuxMaster::controlPath() const { return path; }

bool MuxMaster::isRunning() const { return running.load(); }

bool MuxMaster::isListening() const {
  return running.load() && listenFd >= 0 && acceptNew.load();
}

size_t MuxMaster::activeClientCount() const {
  lock_guard<recursive_mutex> guard(mutex);
  return clients;
}

size_t MuxMaster::sessionCount() const {
  lock_guard<recursive_mutex> guard(mutex);
  return sessions;
}

vector<MuxTrackedForward> MuxMaster::trackedForwards() const {
  lock_guard<recursive_mutex> guard(mutex);
  return forwards;
}

bool MuxMaster::persistExpired() const { return persistDone.load(); }

void MuxMaster::notifyPrimaryClientExited() {
  lock_guard<recursive_mutex> guard(mutex);
  primaryExited = true;
  if (!persistConfig.enabled) {
    if (clients == 0) {
      terminateRequested = true;
    }
    return;
  }
  if (persistConfig.seconds == 0) {
    return;
  }
  persistArmed = true;
  persistDeadline =
      chrono::steady_clock::now() + chrono::seconds(persistConfig.seconds);
}

void MuxMaster::start() {
  if (path.empty()) {
    throw runtime_error("ControlPath is empty");
  }
  if (running.load()) {
    return;
  }

  ::unlink(path.c_str());

  listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listenFd < 0) {
    throw runtime_error("socket(AF_UNIX) failed for ControlPath");
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(listenFd);
    listenFd = -1;
    throw runtime_error("ControlPath too long");
  }
  memcpy(addr.sun_path, path.c_str(), path.size() + 1);
#ifndef WIN32
  socklen_t addrLen =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
#else
  socklen_t addrLen = sizeof(addr);
#endif
  if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), addrLen) < 0) {
    int err = errno;
    ::close(listenFd);
    listenFd = -1;
    throw runtime_error(string("bind ControlPath failed: ") + strerror(err));
  }
  if (::listen(listenFd, 16) < 0) {
    int err = errno;
    ::close(listenFd);
    listenFd = -1;
    ::unlink(path.c_str());
    throw runtime_error(string("listen ControlPath failed: ") + strerror(err));
  }

#ifndef WIN32
  int flags = ::fcntl(listenFd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(listenFd, F_SETFL, flags | O_NONBLOCK);
  }
  ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif

  running = true;
  acceptNew = true;
  terminateRequested = false;
  persistDone = false;
  worker = thread([this]() { acceptLoop(); });
}

void MuxMaster::closeListenFd() {
  lock_guard<recursive_mutex> guard(mutex);
  if (listenFd >= 0) {
    ::shutdown(listenFd, SHUT_RDWR);
    ::close(listenFd);
    listenFd = -1;
  }
}

void MuxMaster::stop() {
  terminateRequested = true;
  running = false;
  acceptNew = false;
  closeListenFd();
  vector<int> clientFds;
  {
    lock_guard<recursive_mutex> guard(mutex);
    clientFds.reserve(clientSlots.size());
    for (const auto& slot : clientSlots) {
      clientFds.push_back(slot.fd);
    }
  }
  for (int fd : clientFds) {
    if (fd >= 0) {
      ::shutdown(fd, SHUT_RDWR);
    }
  }
  if (worker.joinable()) {
    if (worker.get_id() != this_thread::get_id()) {
      worker.join();
    } else {
      worker.detach();
    }
  }
  vector<thread> joining;
  {
    lock_guard<recursive_mutex> guard(mutex);
    for (auto& slot : clientSlots) {
      if (!slot.thr.joinable()) {
        continue;
      }
      if (slot.thr.get_id() == this_thread::get_id()) {
        slot.thr.detach();
      } else {
        joining.push_back(std::move(slot.thr));
      }
    }
    clientSlots.clear();
  }
  for (auto& thr : joining) {
    thr.join();
  }
  if (!path.empty()) {
    ::unlink(path.c_str());
  }
}

void MuxMaster::acceptLoop() {
  while (running.load() && !terminateRequested.load()) {
    {
      lock_guard<recursive_mutex> guard(mutex);
      if (persistArmed && persistConfig.enabled && persistConfig.seconds > 0 &&
          clients == 0 && primaryExited.load()) {
        if (chrono::steady_clock::now() >= persistDeadline) {
          persistDone = true;
          running = false;
          break;
        }
      }
      if (!persistConfig.enabled && primaryExited.load() && clients == 0) {
        running = false;
        break;
      }
    }

    if (!acceptNew.load()) {
      this_thread::sleep_for(chrono::milliseconds(50));
      continue;
    }

    int rc = muxPollFd(listenFd, POLLIN, 100);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (rc == 0) {
      continue;
    }

    int clientFd = ::accept(listenFd, nullptr, nullptr);
    if (clientFd < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == ECONNABORTED) {
        continue;
      }
      break;
    }
#ifndef WIN32
    {
      int flags = ::fcntl(clientFd, F_GETFL, 0);
      if (flags >= 0) {
        ::fcntl(clientFd, F_SETFL, flags & ~O_NONBLOCK);
      }
    }
#endif

    {
      lock_guard<recursive_mutex> guard(mutex);
      ++clients;
      clientSlots.push_back(ClientSlot{});
      ClientSlot& slot = clientSlots.back();
      slot.fd = clientFd;
      slot.thr = thread([this, clientFd]() {
        try {
          serveClient(clientFd);
        } catch (const std::exception& ex) {
          LOG(WARNING) << "Mux client handler failed: " << ex.what();
        }
        ::close(clientFd);
        {
          lock_guard<recursive_mutex> guard(mutex);
          if (clients > 0) {
            --clients;
          }
        }
      });
    }
  }

  running = false;
  closeListenFd();
  ::unlink(path.c_str());
}

void MuxMaster::serveClient(int clientFd) {
  MuxConnection conn(clientFd, false);

  MuxBuffer hello;
  hello.putU32(MUX_MSG_HELLO);
  hello.putU32(SSHMUX_VER);
  if (!conn.writePacket(hello)) {
    return;
  }

  MuxBuffer packet;
  if (!conn.readPacket(&packet, 5000)) {
    return;
  }
  uint32_t type = 0;
  if (!packet.getU32(&type) || type != MUX_MSG_HELLO) {
    return;
  }
  if (!handleHello(&conn, &packet)) {
    return;
  }

  while (running.load() && !terminateRequested.load()) {
    MuxBuffer req;
    if (!conn.readPacket(&req, 200)) {
      if (errno == ETIMEDOUT || errno == EAGAIN) {
        continue;
      }
      break;
    }
    uint32_t reqType = 0;
    if (!req.getU32(&reqType)) {
      break;
    }
    if (reqType == MUX_MSG_HELLO) {
      continue;
    }
    if (!handleRequest(&conn, reqType, &req)) {
      break;
    }
    if (terminateRequested.load()) {
      break;
    }
  }
}

bool MuxMaster::handleHello(MuxConnection* conn, MuxBuffer* body) {
  (void)conn;
  uint32_t ver = 0;
  if (!body->getU32(&ver)) {
    return false;
  }
  if (ver != SSHMUX_VER) {
    LOG(WARNING) << "Unsupported mux protocol version " << ver;
    return false;
  }
  while (body->remaining() > 0) {
    string name;
    string value;
    if (!body->getString(&name) || !body->getString(&value)) {
      return false;
    }
  }
  return true;
}

bool MuxMaster::replyOk(MuxConnection* conn, uint32_t requestId) {
  MuxBuffer reply;
  reply.putU32(MUX_S_OK);
  reply.putU32(requestId);
  return conn->writePacket(reply);
}

bool MuxMaster::replyAlive(MuxConnection* conn, uint32_t requestId) {
  MuxBuffer reply;
  reply.putU32(MUX_S_ALIVE);
  reply.putU32(requestId);
#ifndef WIN32
  reply.putU32(static_cast<uint32_t>(::getpid()));
#else
  reply.putU32(static_cast<uint32_t>(::_getpid()));
#endif
  return conn->writePacket(reply);
}

bool MuxMaster::replyFailure(MuxConnection* conn, uint32_t type,
                             uint32_t requestId, const string& reason) {
  MuxBuffer reply;
  reply.putU32(type);
  reply.putU32(requestId);
  reply.putString(reason);
  return conn->writePacket(reply);
}

bool MuxMaster::replySessionOpened(MuxConnection* conn, uint32_t requestId,
                                   uint32_t sessionId) {
  MuxBuffer reply;
  reply.putU32(MUX_S_SESSION_OPENED);
  reply.putU32(requestId);
  reply.putU32(sessionId);
  return conn->writePacket(reply);
}

bool MuxMaster::openForward(const MuxTrackedForward& fwd, string* error) {
  lock_guard<recursive_mutex> guard(mutex);
  for (const auto& existing : forwards) {
    if (forwardsEqual(existing, fwd)) {
      return true;
    }
  }

  if (portForwardHandler && fwd.type == MUX_FWD_LOCAL) {
    try {
      string tunnel =
          to_string(fwd.listenPort) + ":" + to_string(fwd.connectPort);
      if (!fwd.listenHost.empty() && fwd.listenHost != "localhost" &&
          fwd.listenHost != "127.0.0.1") {
        tunnel = fwd.listenHost + ":" + to_string(fwd.listenPort) + ":" +
                 (fwd.connectHost.empty() ? "localhost" : fwd.connectHost) +
                 ":" + to_string(fwd.connectPort);
      }
      auto requests = parseRangesToRequests(tunnel);
      for (auto& pfsr : requests) {
        auto resp = portForwardHandler->createSource(pfsr, nullptr, -1, -1);
        if (resp.has_error()) {
          *error = resp.error();
          return false;
        }
      }
    } catch (const std::exception& ex) {
      *error = ex.what();
      return false;
    }
  }

  forwards.push_back(fwd);
  return true;
}

bool MuxMaster::closeForward(const MuxTrackedForward& fwd) {
  lock_guard<recursive_mutex> guard(mutex);
  auto it = remove_if(
      forwards.begin(), forwards.end(),
      [&](const MuxTrackedForward& f) { return forwardsEqual(f, fwd); });
  if (it == forwards.end()) {
    return false;
  }
  forwards.erase(it, forwards.end());
  return true;
}

bool MuxMaster::handleRequest(MuxConnection* conn, uint32_t type,
                              MuxBuffer* body) {
  uint32_t requestId = 0;
  if (!body->getU32(&requestId)) {
    return false;
  }

  switch (type) {
    case MUX_C_ALIVE_CHECK:
      return replyAlive(conn, requestId);

    case MUX_C_TERMINATE:
      if (!replyOk(conn, requestId)) {
        return false;
      }
      terminateRequested = true;
      running = false;
      return true;

    case MUX_C_STOP_LISTENING:
      acceptNew = false;
      ::unlink(path.c_str());
      return replyOk(conn, requestId);

    case MUX_C_OPEN_FWD: {
      MuxTrackedForward fwd;
      if (!body->getU32(&fwd.type) || !body->getString(&fwd.listenHost) ||
          !body->getU32(&fwd.listenPort) ||
          !body->getString(&fwd.connectHost) ||
          !body->getU32(&fwd.connectPort)) {
        return replyFailure(conn, MUX_S_FAILURE, requestId, "bad OPEN_FWD");
      }
      string error;
      if (!openForward(fwd, &error)) {
        return replyFailure(conn, MUX_S_FAILURE, requestId,
                            error.empty() ? "open forward failed" : error);
      }
      return replyOk(conn, requestId);
    }

    case MUX_C_CLOSE_FWD: {
      MuxTrackedForward fwd;
      if (!body->getU32(&fwd.type) || !body->getString(&fwd.listenHost) ||
          !body->getU32(&fwd.listenPort) ||
          !body->getString(&fwd.connectHost) ||
          !body->getU32(&fwd.connectPort)) {
        return replyFailure(conn, MUX_S_FAILURE, requestId, "bad CLOSE_FWD");
      }
      if (!closeForward(fwd)) {
        return replyFailure(conn, MUX_S_FAILURE, requestId,
                            "forward not found");
      }
      return replyOk(conn, requestId);
    }

    case MUX_C_NEW_SESSION: {
      string reserved;
      bool wantTty = false;
      bool wantX11 = false;
      bool wantAgent = false;
      bool subsystem = false;
      uint32_t escapeChar = 0;
      string term;
      string command;
      if (!body->getString(&reserved) || !body->getBool(&wantTty) ||
          !body->getBool(&wantX11) || !body->getBool(&wantAgent) ||
          !body->getBool(&subsystem) || !body->getU32(&escapeChar) ||
          !body->getString(&term) || !body->getString(&command)) {
        return replyFailure(conn, MUX_S_FAILURE, requestId, "bad NEW_SESSION");
      }
      while (body->remaining() > 0) {
        string env;
        if (!body->getString(&env)) {
          break;
        }
      }

      // OpenSSH: gather stdio fds before confirming the session.
      int inFd = -1, outFd = -1, errFd = -1;
      if (!conn->recvFd(&inFd) || !conn->recvFd(&outFd) ||
          !conn->recvFd(&errFd)) {
        if (inFd >= 0) {
          ::close(inFd);
        }
        if (outFd >= 0) {
          ::close(outFd);
        }
        if (errFd >= 0) {
          ::close(errFd);
        }
        return replyFailure(conn, MUX_S_FAILURE, requestId,
                            "did not receive file descriptors");
      }

      uint32_t sessionId = 0;
      {
        lock_guard<recursive_mutex> guard(mutex);
        sessionId = static_cast<uint32_t>(nextSessionId++);
        ++sessions;
      }

      if (!replySessionOpened(conn, requestId, sessionId)) {
        ::close(inFd);
        ::close(outFd);
        ::close(errFd);
        return false;
      }

      // Shared command channel is acknowledged on the master; close passenger
      // fds and report a clean exit so attaching clients can finish.
      ::close(inFd);
      ::close(outFd);
      ::close(errFd);

      MuxBuffer exitMsg;
      exitMsg.putU32(MUX_S_EXIT_MESSAGE);
      exitMsg.putU32(sessionId);
      exitMsg.putU32(0);
      conn->writePacket(exitMsg);
      return true;
    }

    case MUX_C_NEW_STDIO_FWD: {
      string reserved;
      string host;
      uint32_t portNum = 0;
      if (!body->getString(&reserved) || !body->getString(&host) ||
          !body->getU32(&portNum)) {
        return replyFailure(conn, MUX_S_FAILURE, requestId,
                            "bad NEW_STDIO_FWD");
      }
      uint32_t sessionId = 0;
      {
        lock_guard<recursive_mutex> guard(mutex);
        sessionId = static_cast<uint32_t>(nextSessionId++);
        ++sessions;
      }
      if (!replySessionOpened(conn, requestId, sessionId)) {
        return false;
      }
      int inFd = -1, outFd = -1;
      if (conn->recvFd(&inFd)) {
        conn->recvFd(&outFd);
        if (inFd >= 0) {
          ::close(inFd);
        }
        if (outFd >= 0) {
          ::close(outFd);
        }
      }
      (void)host;
      (void)portNum;
      return true;
    }

    default:
      return replyFailure(conn, MUX_S_FAILURE, requestId,
                          "unsupported mux request");
  }
}

}  // namespace et
