#include "UnixSocketHandler.hpp"

namespace et {
UnixSocketHandler::UnixSocketHandler() {}

bool UnixSocketHandler::waitForData(int fd, int64_t sec, int64_t usec) {
  fd_set input;
  FD_ZERO(&input);
  FD_SET(fd, &input);
  struct timeval timeout;
  timeout.tv_sec = sec;
  timeout.tv_usec = usec;
  int n = select(fd + 1, &input, NULL, NULL, &timeout);
  if (n == -1) {
    // Select timed out or failed.
    VLOG(4) << "socket select timeout";
    return false;
  } else if (n == 0)
    return false;
  if (!FD_ISSET(fd, &input)) {
    STFATAL << "FD_ISSET is false but we should have data by now.";
  }
  VLOG(4) << "socket " << fd << " has data";
  return true;
}

bool UnixSocketHandler::hasData(int fd) { return waitForData(fd, 0, 0); }

ssize_t UnixSocketHandler::read(int fd, void *buf, size_t count) {
  if (fd <= 0) {
    STFATAL << "Tried to read from an invalid socket: " << fd;
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
  waitForData(fd, 5, 0);
  lock_guard<recursive_mutex> guard(*(it->second));
  VLOG(4) << "Unixsocket handler read from fd: " << fd;
#ifdef WIN32
  ssize_t readBytes = ::recv(fd, (char *)buf, count, 0);
#else
  ssize_t readBytes = ::read(fd, buf, count);
#endif
  auto localErrno = errno;
  if (readBytes < 0 && localErrno != EAGAIN && localErrno != EWOULDBLOCK) {
    LOG(WARNING) << "Error reading: " << localErrno << " "
                 << strerror(localErrno);
  }
  errno = localErrno;
  return readBytes;
}

ssize_t UnixSocketHandler::write(int fd, const void *buf, size_t count) {
  VLOG(4) << "Unixsocket handler write to fd: " << fd;
  if (fd <= 0) {
    STFATAL << "Tried to write to an invalid socket: " << fd;
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
  // Try to write for around 5 seconds before giving up
  time_t startTime = time(NULL);
  int bytesWritten = 0;
  while (bytesWritten < int(count)) {
    lock_guard<recursive_mutex> guard(*(it->second));
    int w;
#ifdef WIN32
    w = ::send(fd, ((const char *)buf) + bytesWritten, count - bytesWritten, 0);
#else
#ifdef MSG_NOSIGNAL
    w = ::send(fd, ((const char *)buf) + bytesWritten, count - bytesWritten,
               MSG_NOSIGNAL);
#else
    w = ::write(fd, ((const char *)buf) + bytesWritten, count - bytesWritten);
#endif
#endif
    if (w < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (time(NULL) > startTime + 5) {
          // Give up
          return -1;
        }
      } else {
        return -1;
      }
    } else {
      bytesWritten += w;
    }
  }
  return count;
}

void UnixSocketHandler::addToActiveSockets(int fd) {
  lock_guard<std::recursive_mutex> guard(globalMutex);
  if (activeSocketMutexes.find(fd) != activeSocketMutexes.end()) {
    STFATAL << "Tried to insert an fd that already exists: " << fd;
  }
  activeSocketMutexes.insert(
      make_pair(fd, shared_ptr<recursive_mutex>(new recursive_mutex())));
}

int UnixSocketHandler::accept(int sockFd) {
  VLOG(3) << "Got mutex when sockethandler accept " << sockFd;
  sockaddr_in client;
  socklen_t c = sizeof(sockaddr_in);
  int client_sock = ::accept(sockFd, (sockaddr *)&client, &c);
  auto acceptErrno = errno;
  while (true) {
    {
      lock_guard<std::recursive_mutex> guard(globalMutex);
      if (activeSocketMutexes.find(client_sock) == activeSocketMutexes.end()) {
        break;
      }
    }
    // Wait until this socket is no longer active
    LOG_EVERY_N(100, INFO) << "Waiting for read/write to time out...";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  lock_guard<std::recursive_mutex> guard(globalMutex);
  VLOG(3) << "Socket " << sockFd
          << " accepted, returned client_sock: " << client_sock;
  if (client_sock >= 0) {
    addToActiveSockets(client_sock);
    lock_guard<recursive_mutex> guard(
        *(activeSocketMutexes.find(client_sock)->second));
    initSocket(client_sock);
    VLOG(3) << "Client_socket inserted to activeSockets";
    return client_sock;
  } else if (acceptErrno != EAGAIN && acceptErrno != EWOULDBLOCK) {
    FATAL_FAIL(-1);  // STFATAL with the error
  }

  errno = acceptErrno;
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
    STERROR << "Tried to close a connection that doesn't exist: " << fd;
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
#if !defined(MSG_NOSIGNAL) && !defined(WIN32)
  {
    // If we don't have MSG_NOSIGNAL, use SO_NOSIGPIPE
    int val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&val, sizeof(val)) ==
        -1) {
      // On Debian + ARM processors, this can fail.  if so, just ignore SIGPIPE
      // globally
      ::signal(SIGPIPE, SIG_IGN);
    }
  }
#endif
  // Also set the accept socket as non-blocking
#ifdef WIN32
  {
    u_long iMode = 1;
    auto result = ioctlsocket(fd, FIONBIO, &iMode);
    if (result != NO_ERROR) {
      STFATAL << result;
    }
  }
#else
  {
    int opts;
    opts = fcntl(fd, F_GETFL);
    FATAL_FAIL_UNLESS_EINVAL(opts);
    opts |= O_NONBLOCK;
    FATAL_FAIL_UNLESS_EINVAL(fcntl(fd, F_SETFL, opts));
  }
#endif
}

void UnixSocketHandler::initServerSocket(int fd) {
  initSocket(fd);
  // Also set the accept socket as reusable
  {
    int flag = 1;
    FATAL_FAIL(
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(int)));
  }
}
}  // namespace et
