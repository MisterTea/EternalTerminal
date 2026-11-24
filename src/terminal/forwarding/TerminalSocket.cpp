#ifdef __linux__
#include "TerminalSocket.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "PipeSocketHandler.hpp"
#include "PortForwardHandler.hpp"

namespace et {
namespace {
constexpr size_t MAX_PENDING_REQUESTS = 64;

template <typename T>
bool sendValue(int socket, T value, int fd) {
  iovec io{&value, sizeof(value)};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  if (fd >= 0) {
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    auto* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
  }
  ssize_t n;
  do {
    n = ::sendmsg(socket, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (n < 0 && errno == EINTR);
  return n == static_cast<ssize_t>(sizeof(value));
}

template <typename T>
ssize_t receiveValue(int socket, T* value, int* fd) {
  iovec io{value, sizeof(*value)};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  // Give the kernel space for exactly one descriptor, excluding tail padding.
  msg.msg_controllen = CMSG_LEN(sizeof(int));
  ssize_t n;
  do {
    n = ::recvmsg(socket, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
  } while (n < 0 && errno == EINTR);
  *fd = -1;
  if (n < 0) return n;
  auto* c = CMSG_FIRSTHDR(&msg);
  if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
      c->cmsg_len == CMSG_LEN(sizeof(int))) {
    memcpy(fd, CMSG_DATA(c), sizeof(*fd));
  }
  const bool invalid = (n != 0 && n != static_cast<ssize_t>(sizeof(*value))) ||
                       (msg.msg_flags & MSG_TRUNC);
  if (invalid || (msg.msg_flags & MSG_CTRUNC)) {
    if (*fd >= 0) ::close(*fd);
    *fd = -1;
  }
  if (invalid) {
    errno = EPROTO;
    return -1;
  }
  return n;
}

int requestTerminalSocket(int routerFd, const std::string& path) {
  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (path.empty() || path.find('\0') != std::string::npos) {
    errno = EINVAL;
    return -1;
  }
  int pair[2];
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0)
    return -1;
  const bool sent = ::send(pair[0], path.data(), path.size(), MSG_NOSIGNAL) ==
                        static_cast<ssize_t>(path.size()) &&
                    sendValue(routerFd, char(TERMINAL_SOCKET_CONNECT), pair[1]);
  const int err = errno;
  ::close(pair[1]);
  if (sent) return pair[0];
  ::close(pair[0]);
  errno = err;
  return -1;
}

int receiveTerminalSocket(int replyFd, int* connectedFd) {
  int error = 0;
  int fd;
  auto n = receiveValue(replyFd, &error, &fd);
  if (n < 0) return -errno;
  if (fd >= 0) {
    sockaddr_un peer{};
    socklen_t size = sizeof(peer);
    int type;
    socklen_t typeSize = sizeof(type);
    if (n == sizeof(error) && error == 0 &&
        ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typeSize) == 0 &&
        type == SOCK_STREAM &&
        ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &size) == 0 &&
        peer.sun_family == AF_UNIX) {
      *connectedFd = fd;
      return 0;
    }
    ::close(fd);
  }
  return n == sizeof(error) && error > 0 ? error : EPROTO;
}

void openRequestedSocket(int replyFd) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ssize_t len;
  do {
    len = ::recv(replyFd, address.sun_path, sizeof(address.sun_path),
                 MSG_TRUNC | MSG_DONTWAIT);
  } while (len < 0 && errno == EINTR);
  if (len <= 0 || static_cast<size_t>(len) >= sizeof(address.sun_path) ||
      memchr(address.sun_path, '\0', len)) {
    sendValue(replyFd, EINVAL, -1);
    return;
  }
  // Skip a queued open if etserver has already abandoned the reply.
  pollfd peer{replyFd, POLLIN, 0};
  int ready;
  do {
    ready = ::poll(&peer, 1, 0);
  } while (ready < 0 && errno == EINTR);
  if (ready != 0) return;

  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  int error = 0;
  if (fd < 0 ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    error = errno;
  sendValue(replyFd, error, error ? -1 : fd);
  if (fd >= 0) ::close(fd);
}
}  // namespace

ssize_t TerminalSocketOpener::readCommand(int routerFd, char* command) {
  int replyFd;
  const auto n = receiveValue(routerFd, command, &replyFd);
  if (replyFd < 0) return n;
  if (n == 1 && *command == TERMINAL_SOCKET_CONNECT) {
    if (pending < MAX_PENDING_REQUESTS) {
      ++pending;
      try {
        if (!worker) worker.emplace(1);
        worker->enqueue([this, replyFd] {
          if (!stopping) openRequestedSocket(replyFd);
          ::close(replyFd);
          --pending;
        });
        return n;
      } catch (const std::exception&) {
        --pending;
      }
    }
    sendValue(replyFd, EAGAIN, -1);
  }
  ::close(replyFd);
  return n;
}

TerminalSocketRequests::TerminalSocketRequests(const TerminalUserInfo& user,
                                               PipeSocketHandler& pipe,
                                               PortForwardHandler& forwards,
                                               Connection& client)
    : routerFd(user.supports_socket_connect() ? user.fd() : -1),
      pipe(pipe),
      forwards(forwards),
      client(client) {}

TerminalSocketRequests::~TerminalSocketRequests() {
  for (const auto& request : pending)
    if (request.replyFd >= 0) ::close(request.replyFd);
}

bool TerminalSocketRequests::tryHandle(PortForwardDestinationRequest& request) {
  if (routerFd < 0 || request.destination().has_port()) return false;
  if (pending.size() >= MAX_PENDING_REQUESTS) {
    respond(request, -1, EAGAIN);
    return true;
  }
  pending.push_back(
      {std::move(request), Clock::now() + std::chrono::seconds(5)});
  return true;
}

void TerminalSocketRequests::respond(
    const PortForwardDestinationRequest& request, int fd, int error) {
  if (fd >= 0) pipe.adoptConnectedSocket(fd);
  auto response = forwards.finishDestination(request, fd, error);
  client.writePacket(
      Packet(PORT_FORWARD_DESTINATION_RESPONSE, protoToString(response)));
}

void TerminalSocketRequests::addFds(std::set<int>& read, std::set<int>& write,
                                    std::set<int>& refresh) const {
  const auto now = Clock::now();
  for (const auto& request : pending) {
    if (request.replyFd >= 0) {
      read.insert(request.replyFd);
      refresh.insert(request.replyFd);
    } else if (request.retryAt <= now) {
      write.insert(routerFd);
    }
  }
}

void TerminalSocketRequests::update(std::set<int>& readable,
                                    const std::set<int>& writable) {
  const auto now = Clock::now();
  bool canSend = writable.count(routerFd);
  for (auto it = pending.begin(); it != pending.end();) {
    auto current = it++;
    int fd = -1;
    int error = ETIMEDOUT;
    if (now < current->deadline) {
      if (current->replyFd < 0) {
        if (!canSend || now < current->retryAt) continue;
        current->replyFd = requestTerminalSocket(
            routerFd, current->destination.destination().name());
        if (current->replyFd >= 0) continue;
        error = errno;
        if (error == EAGAIN) {
          canSend = false;
          continue;
        }
      } else {
        if (!readable.count(current->replyFd)) continue;
        error = receiveTerminalSocket(current->replyFd, &fd);
        if (error == -EAGAIN) continue;
        if (error < 0) error = -error;
      }
    }
    if (current->replyFd >= 0) {
      readable.erase(current->replyFd);
      ::close(current->replyFd);
      if (error == EAGAIN) {
        current->replyFd = -1;
        current->retryAt = now + std::chrono::milliseconds(50);
        continue;
      }
    }
    auto destination = std::move(current->destination);
    pending.erase(current);
    respond(destination, fd, error);
  }
}
}  // namespace et
#endif
