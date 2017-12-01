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
  lock_guard<std::recursive_mutex> guard(mutex);
  fd_set input;
  FD_ZERO(&input);
  FD_SET(fd, &input);
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 0;
  int n = select(fd + 1, &input, NULL, NULL, &timeout);
  if (n == -1) {
    // Select timed out or failed.
    return false;
  } else if (n == 0)
    return false;
  if (!FD_ISSET(fd, &input)) {
    LOG(FATAL) << "FD_ISSET is false but we should have data by now.";
  }
  return true;
}

ssize_t UnixSocketHandler::read(int fd, void *buf, size_t count) {
  lock_guard<std::recursive_mutex> guard(mutex);
  if (fd <= 0) {
    LOG(FATAL) << "Tried to read from an invalid socket: " << fd;
  }
  if (activeSockets.find(fd) == activeSockets.end()) {
    LOG(INFO) << "Tried to read from a socket that has been closed: " << fd;
    return 0;
  }
  ssize_t readBytes = ::read(fd, buf, count);
  if (readBytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    LOG(ERROR) << "Error reading: " << errno << " " << strerror(errno) << endl;
  }
  return readBytes;
}

ssize_t UnixSocketHandler::write(int fd, const void *buf, size_t count) {
  lock_guard<std::recursive_mutex> guard(mutex);
  if (fd <= 0) {
    LOG(FATAL) << "Tried to write to an invalid socket: " << fd;
  }
  if (activeSockets.find(fd) == activeSockets.end()) {
    LOG(INFO) << "Tried to write to a socket that has been closed: " << fd;
    return 0;
  }
#ifdef MSG_NOSIGNAL
  return ::send(fd, buf, count, MSG_NOSIGNAL);
#else
  return ::write(fd, buf, count);
#endif
}

int UnixSocketHandler::connect(const std::string &hostname, int port) {
  lock_guard<std::recursive_mutex> guard(mutex);
  int sockfd = -1;
  addrinfo *results = NULL;
  addrinfo *p = NULL;
  addrinfo hints;
  memset(&hints, 0, sizeof(addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = (AI_CANONNAME | AI_V4MAPPED | AI_ADDRCONFIG | AI_ALL);
  std::string portname = std::to_string(port);

  // (re)initialize the DNS system
  ::res_init();
  int rc = getaddrinfo(hostname.c_str(), portname.c_str(), &hints, &results);

  if (rc == EAI_NONAME) {
    VLOG_EVERY_N(1, 10) << "Cannot resolve hostname: " << gai_strerror(rc);
    if (results) {
      freeaddrinfo(results);
    }
    return -1;
  }

  if (rc != 0) {
    LOG(ERROR) << "Error getting address info for " << hostname << ":"
               << portname << ": " << rc << " (" << gai_strerror(rc) << ")";
    if (results) {
      freeaddrinfo(results);
    }
    return -1;
  }

  // loop through all the results and connect to the first we can
  for (p = results; p != NULL; p = p->ai_next) {
    if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      LOG(INFO) << "Error creating socket: " << errno << " " << strerror(errno);
      continue;
    }
    initSocket(sockfd);

    // Set nonblocking just for the connect phase
    {
      int opts;
      opts = fcntl(sockfd, F_GETFL);
      FATAL_FAIL(opts);
      opts |= O_NONBLOCK;
      FATAL_FAIL(fcntl(sockfd, F_SETFL, opts));
    }
    if (::connect(sockfd, p->ai_addr, p->ai_addrlen) == -1 &&
        errno != EINPROGRESS) {
      if (p->ai_canonname) {
        LOG(INFO) << "Error connecting with " << p->ai_canonname << ": "
                  << errno << " " << strerror(errno);
      } else {
        LOG(INFO) << "Error connecting: " << errno << " " << strerror(errno);
      }
      ::close(sockfd);
      sockfd = -1;
      continue;
    }

    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sockfd, &fdset);
    timeval tv;
    tv.tv_sec = 3; /* 3 second timeout */
    tv.tv_usec = 0;

    if (::select(sockfd + 1, NULL, &fdset, NULL, &tv) == 1) {
      int so_error;
      socklen_t len = sizeof so_error;

      FATAL_FAIL(::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len));

      if (so_error == 0) {
        if (p->ai_canonname) {
          LOG(INFO) << "Connected to server: " << p->ai_canonname
                    << " using fd " << sockfd;
        } else {
          LOG(ERROR) << "Connected to server but canonname is null somehow";
        }
        // Make sure that socket becomes blocking once it's attached to a
        // server.
        {
          int opts;
          opts = fcntl(sockfd, F_GETFL);
          FATAL_FAIL(opts);
          opts &= (~O_NONBLOCK);
          FATAL_FAIL(fcntl(sockfd, F_SETFL, opts));
        }
        break;  // if we get here, we must have connected successfully
      } else {
        if (p->ai_canonname) {
          LOG(INFO) << "Error connecting with " << p->ai_canonname << ": "
                    << so_error << " " << strerror(so_error);
        } else {
          LOG(INFO) << "Error connecting to " << hostname << ": " << so_error
                    << " " << strerror(so_error);
        }
        ::close(sockfd);
        sockfd = -1;
        continue;
      }
    } else {
      if (p->ai_canonname) {
        LOG(INFO) << "Error connecting with " << p->ai_canonname << ": "
                  << errno << " " << strerror(errno);
      } else {
        LOG(INFO) << "Error connecting to " << hostname << ": " << errno << " "
                  << strerror(errno);
      }
      ::close(sockfd);
      sockfd = -1;
      continue;
    }
  }

  if (sockfd == -1) {
    LOG(ERROR) << "ERROR, no host found";
  } else {
    if (activeSockets.find(sockfd) != activeSockets.end()) {
      LOG(FATAL) << "Tried to insert an fd that already exists: " << sockfd;
    }
    activeSockets.insert(sockfd);
  }

  freeaddrinfo(results);
  return sockfd;
}

void UnixSocketHandler::createServerSockets(int port) {
  if (portServerSockets.find(port) != portServerSockets.end()) {
    LOG(FATAL) << "Error: server sockets for port " << port
               << " already exist.";
  }

  addrinfo hints, *servinfo, *p;
  int rc;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;  // use my IP address

  std::string portname = std::to_string(port);

  if ((rc = getaddrinfo(NULL, portname.c_str(), &hints, &servinfo)) != 0) {
    LOG(ERROR) << "Error getting address info for " << port << ": " << rc
               << " (" << gai_strerror(rc) << ")";
    exit(1);
  }

  set<int> serverSockets;
  // loop through all the results and bind to the first we can
  for (p = servinfo; p != NULL; p = p->ai_next) {
    int sockfd;
    if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      LOG(INFO) << "Error creating socket " << p->ai_family << "/"
                << p->ai_socktype << "/" << p->ai_protocol << ": " << errno
                << " " << strerror(errno);
      continue;
    }
    initSocket(sockfd);
    // Also set the accept socket as non-blocking
    {
      int opts;
      opts = fcntl(sockfd, F_GETFL);
      FATAL_FAIL(opts);
      opts |= O_NONBLOCK;
      FATAL_FAIL(fcntl(sockfd, F_SETFL, opts));
    }
    // Also set the accept socket as reusable
    {
      int flag = 1;
      FATAL_FAIL(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag,
                            sizeof(int)));
    }

    if (p->ai_family == AF_INET6) {
      // Also ensure that IPV6 sockets only listen on IPV6
      // interfaces.  We will create another socket object for IPV4
      // if it doesn't already exist.
      int flag = 1;
      FATAL_FAIL(setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&flag,
                            sizeof(int)));
    }

    if (::bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      // This most often happens because the port is in use.
      LOG(ERROR) << "Error binding " << p->ai_family << "/" << p->ai_socktype
                 << "/" << p->ai_protocol << ": " << errno << " "
                 << strerror(errno);
      cerr << "Error binding " << p->ai_family << "/" << p->ai_socktype << "/"
           << p->ai_protocol << ": " << errno << " " << strerror(errno)
           << flush;
      stringstream oss;
      oss << "Error binding port " << port << ": " << errno << " "
          << strerror(errno);
      string s = oss.str();
      throw std::runtime_error(s.c_str());
      // close(sockfd);
      // continue;
    }

    // Listen
    FATAL_FAIL(::listen(sockfd, 32));
    LOG(INFO) << "Listening on "
              << inet_ntoa(((sockaddr_in *)p->ai_addr)->sin_addr) << ":" << port
              << "/" << p->ai_family << "/" << p->ai_socktype << "/"
              << p->ai_protocol;

    // if we get here, we must have connected successfully
    serverSockets.insert(sockfd);
  }

  if (serverSockets.empty()) {
    LOG(FATAL) << "Could not bind to any interface!";
  }

  portServerSockets[port] = serverSockets;
}

void UnixSocketHandler::listen(int port) {
  lock_guard<std::recursive_mutex> guard(mutex);
  if (portServerSockets.find(port) != portServerSockets.end()) {
    LOG(FATAL) << "Tried to listen twice on the same port";
  }
  createServerSockets(port);
}

set<int> UnixSocketHandler::getPortFds(int port) {
  lock_guard<std::recursive_mutex> guard(mutex);
  if (portServerSockets.find(port) == portServerSockets.end()) {
    LOG(FATAL)
        << "Tried to getPortFds on a port without calling listen() first";
  }
  return portServerSockets[port];
}

int UnixSocketHandler::accept(int sockfd) {
  lock_guard<std::recursive_mutex> guard(mutex);
  sockaddr_in client;
  socklen_t c = sizeof(sockaddr_in);
  int client_sock = ::accept(sockfd, (sockaddr *)&client, &c);
  if (client_sock >= 0) {
    initSocket(client_sock);
    activeSockets.insert(client_sock);
    // Make sure that socket becomes blocking once it's attached to a client.
    {
      int opts;
      opts = fcntl(client_sock, F_GETFL);
      FATAL_FAIL(opts);
      opts &= (~O_NONBLOCK);
      FATAL_FAIL(fcntl(client_sock, F_SETFL, opts));
    }
    return client_sock;
  } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
    FATAL_FAIL(-1);  // LOG(FATAL) with the error
  }

  return -1;
}

void UnixSocketHandler::stopListening(int port) {
  lock_guard<std::recursive_mutex> guard(mutex);
  auto it = portServerSockets.find(port);
  if (it == portServerSockets.end()) {
    LOG(FATAL)
        << "Tried to stop listening to a port that we weren't listening on";
  }
  auto &serverSockets = it->second;
  for (int sockfd : serverSockets) {
    close(sockfd);
  }
}

void UnixSocketHandler::close(int fd) {
  lock_guard<std::recursive_mutex> guard(mutex);
  if (fd == -1) {
    return;
  }
  if (activeSockets.find(fd) == activeSockets.end()) {
    // Connection was already killed.
    LOG(ERROR) << "Tried to close a connection that doesn't exist: " << fd;
    return;
  }
  activeSockets.erase(activeSockets.find(fd));
#if 0
  // Shutting down connection before closing to prevent the server
  // from closing it.
  VLOG(1) << "Shutting down connection: " << fd << endl;
  int rc = ::shutdown(fd, SHUT_RDWR);
  if (rc == -1) {
    if (errno == ENOTCONN || errno == EADDRNOTAVAIL) {
      // Shutdown is failing on OS/X with errno (49): Can't assign requested address
      // Possibly an OS bug but I don't think it's necessary anyways.

      // ENOTCONN is harmless
    } else {
      FATAL_FAIL(rc);
    }
  }
#endif
  VLOG(1) << "Closing connection: " << fd << endl;
  FATAL_FAIL(::close(fd));
}

void UnixSocketHandler::initSocket(int fd) {
  int flag = 1;
  FATAL_FAIL(setsockopt(fd,            /* socket affected */
                        IPPROTO_TCP,   /* set option at TCP level */
                        TCP_NODELAY,   /* name of option */
                        (char *)&flag, /* the cast is historical cruft */
                        sizeof(int))); /* length of option value */
  timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  FATAL_FAIL(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv)));
  FATAL_FAIL(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv)));
#ifndef MSG_NOSIGNAL
  // If we don't have MSG_NOSIGNAL, use SO_NOSIGPIPE
  int val = 1;
  FATAL_FAIL(
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&val, sizeof(val)));
#endif
}
}  // namespace et
