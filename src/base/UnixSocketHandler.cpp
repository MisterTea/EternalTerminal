#include "UnixSocketHandler.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <resolv.h>
#include <sys/socket.h>
#include <unistd.h>

namespace et {
UnixSocketHandler::UnixSocketHandler() {}

bool UnixSocketHandler::hasData(int fd) {
  fd_set input;
  FD_ZERO(&input);
  FD_SET(fd, &input);
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 0;
  int n = select(fd + 1, &input, NULL, NULL, &timeout);
  if (n == -1) {
    // Select timed out or failed.
    VLOG(4) << "socket select timeout";
    return false;
  } else if (n == 0)
    return false;
  if (!FD_ISSET(fd, &input)) {
    LOG(FATAL) << "FD_ISSET is false but we should have data by now.";
  }
  VLOG(4) << "socket " << fd << " has data";
  return true;
}

ssize_t UnixSocketHandler::read(int fd, void *buf, size_t count) {
  if (fd <= 0) {
    LOG(FATAL) << "Tried to read from an invalid socket: " << fd;
  }
  map<int, shared_ptr<recursive_mutex>>::iterator it;
  {
    lock_guard<std::recursive_mutex> guard(globalMutex);
    it = activeSocketMutexes.find(fd);
    if (it == activeSocketMutexes.end()) {
      LOG(INFO) << "Tried to read from a socket that has been closed: " << fd;
      errno = EPIPE;
      return -1;
    }
  }
  lock_guard<recursive_mutex> guard(*(it->second));
  VLOG(4) << "Unixsocket handler read from fd: " << fd;
  ssize_t readBytes = ::read(fd, buf, count);
  if (readBytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    LOG(ERROR) << "Error reading: " << errno << " " << strerror(errno);
  }
  return readBytes;
}

ssize_t UnixSocketHandler::write(int fd, const void *buf, size_t count) {
  VLOG(4) << "Unixsocket handler write to fd: " << fd;
  if (fd <= 0) {
    LOG(FATAL) << "Tried to write to an invalid socket: " << fd;
  }
  map<int, shared_ptr<recursive_mutex>>::iterator it;
  {
    lock_guard<std::recursive_mutex> guard(globalMutex);
    it = activeSocketMutexes.find(fd);
    if (it == activeSocketMutexes.end()) {
      LOG(INFO) << "Tried to write to a socket that has been closed: " << fd;
      errno = EPIPE;
      return -1;
    }
  }
  lock_guard<recursive_mutex> guard(*(it->second));
#ifdef MSG_NOSIGNAL
  return ::send(fd, buf, count, MSG_NOSIGNAL);
#else
  return ::write(fd, buf, count);
#endif
}

void UnixSocketHandler::addToActiveSockets(int fd) {
  lock_guard<std::recursive_mutex> guard(globalMutex);
  if (activeSocketMutexes.find(fd) != activeSocketMutexes.end()) {
    LOG(FATAL) << "Tried to insert an fd that already exists: " << fd;
  }
  activeSocketMutexes.insert(
      make_pair(fd, shared_ptr<recursive_mutex>(new recursive_mutex())));
}

int UnixSocketHandler::accept(int sockFd) {
  VLOG(3) << "Got mutex when sockethandler accept " << sockFd;
  sockaddr_in client;
  socklen_t c = sizeof(sockaddr_in);
  int client_sock = ::accept(sockFd, (sockaddr *)&client, &c);
  while (true) {
    {
      lock_guard<std::recursive_mutex> guard(globalMutex);
      if (activeSocketMutexes.find(client_sock) == activeSocketMutexes.end()) {
        break;
      }
    }
    // Wait until this socket is no longer active
    LOG_EVERY_N(100, INFO) << "Waiting for read/write to time out...";
    usleep(1 * 1000);
  }
  VLOG(3) << "Socket " << sockFd
          << " accepted, returned client_sock: " << client_sock;
  if (client_sock >= 0) {
    // Make sure that socket becomes blocking once it's attached to a client.
    {
      int opts;
      opts = fcntl(client_sock, F_GETFL, 0);
      FATAL_FAIL(opts);
      opts &= (~O_NONBLOCK);
      FATAL_FAIL(fcntl(client_sock, F_SETFL, opts));
    }
    initSocket(client_sock);
    addToActiveSockets(client_sock);
    VLOG(3) << "Client_socket inserted to activeSockets";
    return client_sock;
  } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
    FATAL_FAIL(-1);  // LOG(FATAL) with the error
  }

  return -1;
}

void UnixSocketHandler::close(int fd) {
  lock_guard<std::recursive_mutex> globalGuard(globalMutex);
  if (fd == -1) {
    return;
  }
  auto it = activeSocketMutexes.find(fd);
  if (it == activeSocketMutexes.end()) {
    // Connection was already killed.
    LOG(ERROR) << "Tried to close a connection that doesn't exist: " << fd;
    return;
  }
  auto m = it->second;
  lock_guard<std::recursive_mutex> guard(*m);
  VLOG(1) << "Closing connection: " << fd;
  FATAL_FAIL(::close(fd));
  activeSocketMutexes.erase(it);
}

vector<int> UnixSocketHandler::getActiveSockets() {
  vector<int> fds;
  for (auto it : activeSocketMutexes) {
    fds.push_back(it.first);
  }
  return fds;
}

void UnixSocketHandler::initSocket(int fd) {
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  FATAL_FAIL_UNLESS_EINVAL(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv,
                                      sizeof(struct timeval)));
  FATAL_FAIL_UNLESS_EINVAL(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv,
                                      sizeof(struct timeval)));
  // Set linger
  struct linger so_linger;
  so_linger.l_onoff = 1;
  so_linger.l_linger = 10;
  int z =
      setsockopt(fd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof so_linger);
  if (z) {
    LOG(FATAL) << "set socket linger failed";
  }
#ifndef MSG_NOSIGNAL
  // If we don't have MSG_NOSIGNAL, use SO_NOSIGPIPE
  int val = 1;
  FATAL_FAIL_UNLESS_EINVAL(
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&val, sizeof(val)));
#endif
}

void UnixSocketHandler::initServerSocket(int fd) {
  initSocket(fd);
  // Also set the accept socket as non-blocking
  {
    int opts;
    opts = fcntl(fd, F_GETFL);
    FATAL_FAIL_UNLESS_EINVAL(opts);
    opts |= O_NONBLOCK;
    FATAL_FAIL_UNLESS_EINVAL(fcntl(fd, F_SETFL, opts));
  }
  // Also set the accept socket as reusable
  {
    int flag = 1;
    FATAL_FAIL_UNLESS_EINVAL(
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(int)));
  }
}
}  // namespace et
