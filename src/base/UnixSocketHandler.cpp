#include "UnixSocketHandler.hpp"

#include <cstdint>

// How long a write blocked on EAGAIN keeps retrying before it gives up, when
// NOTHING of the buffer has reached the wire yet. Unchanged from the original
// hard-coded 5: at this point giving up is clean, because the peer has seen
// none of the message and the caller's -1 is the whole truth.
#define SOCKET_WRITE_TIMEOUT (5)

// The same, once part of the buffer HAS been sent and cannot be recalled.
// Deliberately far longer: giving up here truncates the message rather than
// failing it (see the comment at the check). A minute of a completely
// undrainable socket means the connection is dead, at which point it is being
// torn down anyway and a truncated message no longer matters.
#define SOCKET_WRITE_COMMITTED_TIMEOUT (60)

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

ssize_t UnixSocketHandler::read(int fd, void* buf, size_t count) {
  if (fd <= 0) {
    STFATAL << "Tried to read from an invalid socket: " << fd;
  }
  map<int, shared_ptr<recursive_mutex>>::iterator it;
  {
    lock_guard<std::recursive_mutex> guard(globalMutex);
    it = activeSocketMutexes.find(fd);
    if (it == activeSocketMutexes.end()) {
      LOG(INFO) << "Tried to read from a socket that has been closed: " << fd;
      SetErrno(EPIPE);
      return -1;
    }
  }
  waitForData(fd, 5, 0);
  lock_guard<recursive_mutex> guard(*(it->second));
  VLOG(4) << "Unixsocket handler read from fd: " << fd;
#ifdef WIN32
  ssize_t readBytes = ::recv(fd, (char*)buf, count, 0);
#else
  ssize_t readBytes = ::read(fd, buf, count);
#endif
  auto localErrno = GetErrno();
  if (readBytes < 0 && localErrno != EAGAIN && localErrno != EWOULDBLOCK) {
    LOG(WARNING) << "Error reading: " << localErrno << " "
                 << strerror(localErrno);
  }
  SetErrno(localErrno);
  return readBytes;
}

ssize_t UnixSocketHandler::write(int fd, const void* buf, size_t count) {
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
      SetErrno(EPIPE);
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
    w = ::send(fd, ((const char*)buf) + bytesWritten, count - bytesWritten, 0);
#else
#ifdef MSG_NOSIGNAL
    w = ::send(fd, ((const char*)buf) + bytesWritten, count - bytesWritten,
               MSG_NOSIGNAL);
#else
    w = ::write(fd, ((const char*)buf) + bytesWritten, count - bytesWritten);
#endif
#endif
    auto localErrno = GetErrno();
    if (w < 0) {
      if (localErrno == EAGAIN || localErrno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // Giving up once part of the buffer is already on the wire truncates
        // the message. `send` returns a short count whenever the send buffer
        // has room for some but not all of it, so `bytesWritten > 0` with the
        // socket now blocking is an ordinary state, not a rare one. Returning
        // -1 there is ambiguous: it cannot be told apart from "nothing was
        // sent", so the caller has no way to know how much of the buffer the
        // peer actually holds.
        //
        // Callers survive that ambiguity only by throwing the connection away.
        // BackedWriter treats -1 as WROTE_WITH_FAILURE, and Connection turns
        // that into closeSocketAndMaybeReconnect(), replaying unacknowledged
        // messages from its backup buffer. That is correct, but it pays for a
        // full reconnect every time ordinary backpressure trips the deadline.
        //
        // So the 5s budget applies only while nothing is committed, where -1
        // is unambiguous and giving up is cheap. Past that point the write
        // should be allowed to finish rather than force a teardown, and only
        // an outright dead connection (a real error from `send`, handled
        // below, or the long ceiling here) ends it.
        const time_t budget = bytesWritten > 0 ? SOCKET_WRITE_COMMITTED_TIMEOUT
                                               : SOCKET_WRITE_TIMEOUT;
        if (time(NULL) > startTime + budget) {
          if (bytesWritten > 0) {
            LOG(ERROR) << "Truncated write on fd " << fd << ": sent "
                       << bytesWritten << " of " << count
                       << " bytes before the socket stayed unwritable for "
                       << SOCKET_WRITE_COMMITTED_TIMEOUT
                       << "s. The peer holds a partial message; this write is "
                          "reported as failed, so the caller must discard the "
                          "connection rather than retry on it.";
          }
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
  sockaddr_storage client;
  socklen_t c = sizeof(client);
  int client_sock = ::accept(sockFd, (sockaddr*)&client, &c);
  auto acceptErrno = GetErrno();
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
  if (client_sock >= 0) {
    VLOG(3) << "Socket " << sockFd
            << " accepted, returned client_sock: " << client_sock;
    addToActiveSockets(client_sock);
    lock_guard<recursive_mutex> guard(
        *(activeSocketMutexes.find(client_sock)->second));
    initSocket(client_sock);
    VLOG(3) << "Client_socket inserted to activeSockets";
    return client_sock;
  } else if (isTransientAcceptError(acceptErrno)) {
    // Transient, per-connection failure: fall through and return -1; the
    // server loop simply retries on the next iteration.
  } else {
    FATAL_FAIL(-1);  // STFATAL with the error
  }

  SetErrno(acceptErrno);
  return -1;
}

bool UnixSocketHandler::isTransientAcceptError(int err) {
  // accept(2) routinely fails for benign, per-connection reasons that must
  // not abort the whole server:
  //  - EAGAIN/EWOULDBLOCK: non-blocking socket with no pending connection.
  //  - ECONNABORTED: the peer reset the connection between landing in the
  //    listen queue and our accept() call.  Surfaced readily on FreeBSD by
  //    clients that connect and immediately disconnect (keepalive/reconnect
  //    churn) and previously aborted etserver.
  //  - EINTR: the call was interrupted by a signal.
  return err == EAGAIN || err == EWOULDBLOCK || err == ECONNABORTED ||
         err == EINTR;
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
  setBlocking(fd, true);
#ifdef _MSC_VER
  FATAL_FAIL_UNLESS_ZERO(::closesocket(fd));
#else
#ifdef __FreeBSD__
  FATAL_FAIL_UNLESS_EAGAIN(::close(fd));
#else
  FATAL_FAIL(::close(fd));
#endif
#endif
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
#if !defined(WIN32)
  // ignore SIGPIPE globally
  ::signal(SIGPIPE, SIG_IGN);
#endif
  // Also set the accept socket as non-blocking
  setBlocking(fd, false);
}

void UnixSocketHandler::initServerSocket(int fd) {
  initSocket(fd);
  // Also set the accept socket as reusable
  {
    int flag = 1;
    FATAL_FAIL(
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&flag, sizeof(int)));
  }
}

void UnixSocketHandler::setBlocking(int sockFd, bool blocking) {
#ifdef WIN32
  {
    u_long iMode = u_long(!blocking);
    auto result = ioctlsocket(sockFd, FIONBIO, &iMode);
    if (result != NO_ERROR) {
      STFATAL << result;
    }
  }
#else
  {
    int opts;
    opts = fcntl(sockFd, F_GETFL);
    FATAL_FAIL(opts);
    if (blocking) {
      opts &= (~O_NONBLOCK);
    } else {
      opts |= O_NONBLOCK;
    }
    FATAL_FAIL(fcntl(sockFd, F_SETFL, opts));
  }
#endif
}
}  // namespace et
