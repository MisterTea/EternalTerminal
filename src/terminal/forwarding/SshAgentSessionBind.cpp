#include "SshAgentSessionBind.hpp"

#ifndef WIN32
#include <poll.h>
#endif

namespace et {
namespace {
constexpr uint8_t SSH_AGENT_FAILURE = 5;
constexpr uint8_t SSH_AGENT_SUCCESS = 6;
constexpr uint8_t SSH_AGENTC_EXTENSION = 27;
const string SESSION_BIND_EXTENSION = "session-bind@openssh.com";
// Only warns: the agent may be waiting on the user to confirm a key.
constexpr int SLOW_AGENT_WARNING_MS = 5000;

uint32_t getU32(const char* p) {
  return (uint32_t(uint8_t(p[0])) << 24) | (uint32_t(uint8_t(p[1])) << 16) |
         (uint32_t(uint8_t(p[2])) << 8) | uint32_t(uint8_t(p[3]));
}

string frame(const string& body) {
  uint32_t length = body.size();
  string framed = {char(length >> 24), char(length >> 16), char(length >> 8),
                   char(length)};
  framed += body;
  return framed;
}

bool readString(const string& message, size_t* pos, string* out) {
  if (message.size() - *pos < 4) {
    return false;
  }
  uint32_t length = getU32(message.data() + *pos);
  *pos += 4;
  if (message.size() - *pos < length) {
    return false;
  }
  out->assign(message, *pos, length);
  *pos += length;
  return true;
}

#ifndef WIN32
const string PROXY_SOCKET_NAME = "agent.sock";

void setCloseOnExec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }

// Proxy directories in use. The SIGINT/SIGTERM handlers call exit(), which
// skips the recorder's destructor, so an atexit hook removes them instead.
std::mutex activeProxyMutex;

set<string>& activeProxyDirectories() {
  static set<string> directories;
  return directories;
}

void removeActiveProxies() {
  // A signal may arrive while the main thread holds the lock.
  std::unique_lock<std::mutex> guard(activeProxyMutex, std::try_to_lock);
  if (!guard.owns_lock()) {
    return;
  }
  for (const auto& directory : activeProxyDirectories()) {
    ::unlink((directory + "/" + PROXY_SOCKET_NAME).c_str());
    ::rmdir(directory.c_str());
  }
}

void trackProxyDirectory(const string& directory, bool active) {
  // Construct the set before registering the hook so it outlives the hook.
  auto& directories = activeProxyDirectories();
  static std::once_flag registered;
  std::call_once(registered, [] { atexit(removeActiveProxies); });
  lock_guard<std::mutex> guard(activeProxyMutex);
  if (active) {
    directories.insert(directory);
  } else {
    directories.erase(directory);
  }
}

void closeIfOpen(int* fd) {
  if (*fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}

bool sendAll(int fd, const string& data) {
  size_t pos = 0;
  while (pos < data.size()) {
#ifdef MSG_NOSIGNAL
    ssize_t written =
        ::send(fd, data.data() + pos, data.size() - pos, MSG_NOSIGNAL);
#else
    ssize_t written = ::write(fd, data.data() + pos, data.size() - pos);
#endif
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    pos += written;
  }
  return true;
}
#endif
}  // namespace

/** @brief Host key blob of a well-formed session-bind, and its forwarding
 * flag. */
static optional<pair<string, bool>> parseSessionBind(const string& request) {
  if (request.empty() || uint8_t(request[0]) != SSH_AGENTC_EXTENSION) {
    return nullopt;
  }
  size_t pos = 1;
  string name, hostKey, sessionId, signature;
  if (!readString(request, &pos, &name) || name != SESSION_BIND_EXTENSION ||
      !readString(request, &pos, &hostKey) ||
      !readString(request, &pos, &sessionId) ||
      !readString(request, &pos, &signature) || pos + 1 != request.size() ||
      uint8_t(request[pos]) > 1) {
    return nullopt;
  }
  if (hostKey.empty() || sessionId.empty() || signature.empty()) {
    return nullopt;
  }
  return make_pair(hostKey, request[pos] == 1);
}

optional<string> forwardedSessionBind(const string& request) {
  auto bind = parseSessionBind(request);
  if (!bind || bind->second) {
    return nullopt;
  }
  string forwarded = request;
  forwarded.back() = 1;
  return forwarded;
}

optional<string> sessionBindHostKeyFingerprint(const string& bind) {
  auto parsed = parseSessionBind(bind);
  if (!parsed) {
    return nullopt;
  }
  unsigned char digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, (const unsigned char*)parsed->first.data(),
                     parsed->first.size());
  string encoded;
  Base64::Encode(string((const char*)digest, sizeof(digest)), &encoded);
  while (!encoded.empty() && encoded.back() == '=') {
    encoded.pop_back();
  }
  return "SHA256:" + encoded;
}

bool sendAgentSessionBinds(SocketHandler* socketHandler, int fd,
                           const string& agentSocketPath,
                           const vector<string>& binds) {
  string requests;
  for (const auto& bind : binds) {
    requests += frame(bind);
  }
  if (socketHandler->writeAllOrReturn(fd, requests.data(), requests.size()) !=
      int(requests.size())) {
    LOG(WARNING) << "Could not send " << binds.size()
                 << " session binding(s) to ssh agent " << agentSocketPath
                 << ": " << strerror(GetErrno());
    return false;
  }
  return true;
}

AgentSessionBindReplies::AgentSessionBindReplies(size_t bindCount,
                                                 const string& agentSocketPath)
    : bindCount_(bindCount), agentSocketPath_(agentSocketPath) {}

bool AgentSessionBindReplies::consume(const string& data, string* forward) {
  if (repliesSeen_ == bindCount_) {
    *forward = data;
    return true;
  }
  pending_ += data;
  forward->clear();
  while (repliesSeen_ < bindCount_ && pending_.size() >= 4) {
    uint32_t length = getU32(pending_.data());
    if (length == 0 || length > SSH_AGENT_MAX_MESSAGE_LENGTH) {
      LOG(WARNING) << "ssh agent " << agentSocketPath_
                   << " sent an invalid reply to session binding "
                   << repliesSeen_ + 1 << "/" << bindCount_ << " (length "
                   << length << ")";
      return false;
    }
    if (pending_.size() < 4 + size_t(length)) {
      break;
    }
    repliesSeen_++;
    uint8_t type = pending_[4];
    if (type == SSH_AGENT_SUCCESS) {
      VLOG(1) << "ssh agent " << agentSocketPath_
              << " accepted session binding " << repliesSeen_ << "/"
              << bindCount_;
    } else {
      // Same as ssh: agents without the extension keep working, but
      // destination-constrained keys stay unusable on this connection.
      LOG(INFO) << "ssh agent " << agentSocketPath_
                << " refused session binding " << repliesSeen_ << "/"
                << bindCount_ << " (reply type " << int(type) << ")";
    }
    pending_.erase(0, 4 + size_t(length));
  }
  if (repliesSeen_ == bindCount_) {
    forward->swap(pending_);
    pending_.clear();
  }
  return true;
}

#ifndef WIN32
SshAgentSessionBindRecorder::SshAgentSessionBindRecorder(
    const string& upstreamAgentPath)
    : upstreamAgentPath_(upstreamAgentPath) {
  // Distinct from TerminalClient's long-lived et-agent-<id> proxy directory.
  string pattern =
      (fs::path(GetTempDirectory()) / "et-ssh-bind-XXXXXX").string();
  vector<char> directory(pattern.begin(), pattern.end());
  directory.push_back('\0');
  // mkdtemp creates the directory 0700, so only this user can reach the proxy.
  if (mkdtemp(directory.data()) == nullptr) {
    throw std::runtime_error(string("mkdtemp failed: ") + strerror(errno));
  }
  directory_ = directory.data();
  socketPath_ = directory_ + "/" + PROXY_SOCKET_NAME;

  sockaddr_un local = {};
  local.sun_family = AF_UNIX;
  string error;
  if (socketPath_.size() >= sizeof(local.sun_path)) {
    error = "agent proxy path too long: " + socketPath_;
  } else {
    strncpy(local.sun_path, socketPath_.c_str(), sizeof(local.sun_path) - 1);
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0 ||
        ::bind(listenFd_, (sockaddr*)&local, sizeof(local)) != 0 ||
        ::listen(listenFd_, 8) != 0 || ::pipe(stopPipe_) != 0) {
      error = string("agent proxy socket setup failed: ") + strerror(errno);
    }
  }
  if (!error.empty()) {
    closeIfOpen(&listenFd_);
    closeIfOpen(&stopPipe_[0]);
    closeIfOpen(&stopPipe_[1]);
    ::unlink(socketPath_.c_str());
    ::rmdir(directory_.c_str());
    throw std::runtime_error(error);
  }
  setCloseOnExec(listenFd_);
  setCloseOnExec(stopPipe_[0]);
  setCloseOnExec(stopPipe_[1]);
  trackProxyDirectory(directory_, true);
  if (upstreamAgentPath_.empty()) {
    LOG(WARNING) << "ssh has no agent to authenticate with; agent proxy "
                 << socketPath_ << " answers every request with failure";
  } else {
    LOG(INFO) << "Agent proxy " << socketPath_ << " relays to ssh agent "
              << upstreamAgentPath_;
  }
  // Keep signals on the main thread: ET's SIGINT/SIGTERM handlers call exit(),
  // which must not run on two threads at once. The thread inherits the mask.
  sigset_t allSignals, previousMask;
  sigfillset(&allSignals);
  pthread_sigmask(SIG_SETMASK, &allSignals, &previousMask);
  worker_ = std::thread(&SshAgentSessionBindRecorder::run, this);
  pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
}

SshAgentSessionBindRecorder::~SshAgentSessionBindRecorder() {
  char stop = 0;
  while (::write(stopPipe_[1], &stop, 1) < 0 && errno == EINTR) {
  }
  worker_.join();
  closeIfOpen(&listenFd_);
  closeIfOpen(&stopPipe_[0]);
  closeIfOpen(&stopPipe_[1]);
  ::unlink(socketPath_.c_str());
  ::rmdir(directory_.c_str());
  trackProxyDirectory(directory_, false);
}

optional<string> SshAgentSessionBindRecorder::lastForwardedBind() {
  lock_guard<std::mutex> guard(mutex_);
  return lastForwardedBind_;
}

void SshAgentSessionBindRecorder::run() {
  vector<Client> clients;
  while (true) {
    vector<pollfd> pollFds = {{stopPipe_[0], POLLIN, 0},
                              {listenFd_, POLLIN, 0}};
    for (const auto& client : clients) {
      pollFds.push_back({client.fd, POLLIN, 0});
    }
    if (::poll(pollFds.data(), pollFds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      STERROR << "agent proxy poll failed: " << strerror(errno);
      break;
    }
    if (pollFds[0].revents) {
      break;
    }
    vector<Client> active;
    for (size_t i = 0; i < clients.size(); i++) {
      if (pollFds[i + 2].revents && !serviceClient(&clients[i])) {
        closeIfOpen(&clients[i].fd);
        closeIfOpen(&clients[i].upstreamFd);
      } else {
        active.push_back(std::move(clients[i]));
      }
    }
    clients = std::move(active);
    if (pollFds[1].revents & POLLIN) {
      int fd = ::accept(listenFd_, nullptr, nullptr);
      if (fd >= 0) {
        setCloseOnExec(fd);
        clients.push_back({fd, connectUpstream(), ""});
      }
    }
  }
  for (auto& client : clients) {
    closeIfOpen(&client.fd);
    closeIfOpen(&client.upstreamFd);
  }
}

bool SshAgentSessionBindRecorder::serviceClient(Client* client) {
  char buf[4096];
  ssize_t bytesRead = ::read(client->fd, buf, sizeof(buf));
  if (bytesRead < 0 && errno == EINTR) {
    return true;
  }
  if (bytesRead <= 0) {
    return false;
  }
  client->buffer.append(buf, bytesRead);
  while (client->buffer.size() >= 4) {
    uint32_t length = getU32(client->buffer.data());
    if (length == 0 || length > SSH_AGENT_MAX_MESSAGE_LENGTH) {
      return false;
    }
    if (client->buffer.size() < 4 + size_t(length)) {
      break;
    }
    string request = client->buffer.substr(4, length);
    client->buffer.erase(0, 4 + size_t(length));
    auto bind = forwardedSessionBind(request);
    if (bind) {
      lock_guard<std::mutex> guard(mutex_);
      lastForwardedBind_ = *bind;
      VLOG(1) << "Agent proxy saw session binding for host key "
              << sessionBindHostKeyFingerprint(*bind).value_or("?");
    }
    string reply;
    if (!relay(client, request, &reply) || !sendAll(client->fd, frame(reply))) {
      return false;
    }
  }
  return true;
}

bool SshAgentSessionBindRecorder::relay(Client* client, const string& request,
                                        string* reply) {
  if (client->upstreamFd < 0) {
    *reply = string(1, char(SSH_AGENT_FAILURE));
    return true;
  }
  return sendAll(client->upstreamFd, frame(request)) &&
         readFrame(client->upstreamFd, reply);
}

bool SshAgentSessionBindRecorder::readFrame(int fd, string* body) {
  bool warnedSlow = false;
  char lengthBytes[4];
  if (!readExact(fd, lengthBytes, sizeof(lengthBytes), &warnedSlow)) {
    return false;
  }
  uint32_t length = getU32(lengthBytes);
  if (length == 0 || length > SSH_AGENT_MAX_MESSAGE_LENGTH) {
    LOG(WARNING) << "ssh agent " << upstreamAgentPath_
                 << " sent an invalid reply (length " << length << ")";
    return false;
  }
  body->assign(length, '\0');
  return readExact(fd, &(*body)[0], length, &warnedSlow);
}

bool SshAgentSessionBindRecorder::readExact(int fd, char* buf, size_t count,
                                            bool* warnedSlow) {
  size_t pos = 0;
  while (pos < count) {
    // Never gives up: the agent may be waiting on the user to confirm a key
    // (ssh-add -c). Shutdown still interrupts through the stop pipe.
    pollfd pollFds[2] = {{fd, POLLIN, 0}, {stopPipe_[0], POLLIN, 0}};
    int ready = ::poll(pollFds, 2, *warnedSlow ? -1 : SLOW_AGENT_WARNING_MS);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (ready == 0) {
      *warnedSlow = true;
      LOG(WARNING) << "ssh agent " << upstreamAgentPath_
                   << " has not answered for " << SLOW_AGENT_WARNING_MS / 1000
                   << "s; still waiting (it may be asking to confirm a key)";
      continue;
    }
    if (pollFds[1].revents) {
      return false;
    }
    ssize_t bytesRead = ::read(fd, buf + pos, count - pos);
    if (bytesRead < 0 && errno == EINTR) {
      continue;
    }
    if (bytesRead <= 0) {
      return false;
    }
    pos += bytesRead;
  }
  return true;
}

int SshAgentSessionBindRecorder::connectUpstream() {
  if (upstreamAgentPath_.empty()) {
    return -1;
  }
  sockaddr_un remote = {};
  remote.sun_family = AF_UNIX;
  if (upstreamAgentPath_.size() >= sizeof(remote.sun_path)) {
    LOG(WARNING) << "ssh agent path too long: " << upstreamAgentPath_;
    return -1;
  }
  strncpy(remote.sun_path, upstreamAgentPath_.c_str(),
          sizeof(remote.sun_path) - 1);
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  setCloseOnExec(fd);
  if (::connect(fd, (sockaddr*)&remote, sizeof(remote)) != 0) {
    LOG(WARNING) << "Could not connect to ssh agent " << upstreamAgentPath_
                 << ": " << strerror(errno);
    ::close(fd);
    return -1;
  }
  return fd;
}
#endif

}  // namespace et
