#include "TcpSocketHandler.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <resolv.h>
#include <sys/socket.h>
#include <unistd.h>

namespace et {
TcpSocketHandler::TcpSocketHandler() {}

int TcpSocketHandler::connect(const SocketEndpoint &endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);
  int sockFd = -1;
  addrinfo *results = NULL;
  addrinfo *p = NULL;
  addrinfo hints;
  memset(&hints, 0, sizeof(addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
#if __NetBSD__
  hints.ai_flags = (AI_CANONNAME | AI_ADDRCONFIG);
#else
  hints.ai_flags = (AI_CANONNAME | AI_V4MAPPED | AI_ADDRCONFIG | AI_ALL);
#endif
  std::string portname = std::to_string(endpoint.getPort());
  std::string hostname = endpoint.getName();

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
    LOG(ERROR) << "Error getting address info for " << endpoint << ": " << rc
               << " (" << gai_strerror(rc) << ")";
    if (results) {
      freeaddrinfo(results);
    }
    return -1;
  }

  // loop through all the results and connect to the first we can
  for (p = results; p != NULL; p = p->ai_next) {
    if ((sockFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      LOG(INFO) << "Error creating socket: " << errno << " " << strerror(errno);
      continue;
    }

    // Set nonblocking just for the connect phase
    {
      int opts;
      opts = fcntl(sockFd, F_GETFL);
      FATAL_FAIL(opts);
      opts |= O_NONBLOCK;
      FATAL_FAIL(fcntl(sockFd, F_SETFL, opts));
      // Set linger
      struct linger so_linger;
      so_linger.l_onoff = 1;
      so_linger.l_linger = 5;
      int z = setsockopt(sockFd, SOL_SOCKET, SO_LINGER, &so_linger,
                         sizeof so_linger);
      if (z) {
        LOG(FATAL) << "set socket linger failed";
      }
    }
    VLOG(4) << "Set nonblocking";
    if (::connect(sockFd, p->ai_addr, p->ai_addrlen) == -1 &&
        errno != EINPROGRESS) {
      if (p->ai_canonname) {
        LOG(INFO) << "Error connecting with " << p->ai_canonname << ": "
                  << errno << " " << strerror(errno);
      } else {
        LOG(INFO) << "Error connecting: " << errno << " " << strerror(errno);
      }
      ::close(sockFd);
      sockFd = -1;
      continue;
    }
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sockFd, &fdset);
    timeval tv;
    tv.tv_sec = 3; /* 3 second timeout */
    tv.tv_usec = 0;
    VLOG(4) << "Before selecting sockFd";
    select(sockFd + 1, NULL, &fdset, NULL, &tv);

    if (FD_ISSET(sockFd, &fdset)) {
      VLOG(4) << "sockFd " << sockFd << "is selected" << sockFd;
      int so_error;
      socklen_t len = sizeof so_error;

      FATAL_FAIL(::getsockopt(sockFd, SOL_SOCKET, SO_ERROR, &so_error, &len));

      if (so_error == 0) {
        if (p->ai_canonname) {
          LOG(INFO) << "Connected to server: " << p->ai_canonname
                    << " using fd " << sockFd;
        } else {
          LOG(ERROR) << "Connected to server but canonname is null somehow";
        }
        // Make sure that socket becomes blocking once it's attached to a
        // server.
        {
          int opts;
          opts = fcntl(sockFd, F_GETFL);
          FATAL_FAIL(opts);
          opts &= (~O_NONBLOCK);
          FATAL_FAIL(fcntl(sockFd, F_SETFL, opts));
        }
        break;  // if we get here, we must have connected successfully
      } else {
        if (p->ai_canonname) {
          LOG(INFO) << "Error connecting with " << p->ai_canonname << ": "
                    << so_error << " " << strerror(so_error);
        } else {
          LOG(INFO) << "Error connecting to " << endpoint << ": " << so_error
                    << " " << strerror(so_error);
        }
        ::close(sockFd);
        sockFd = -1;
        continue;
      }
    } else {
      if (p->ai_canonname) {
        LOG(INFO) << "Error connecting with " << p->ai_canonname << ": "
                  << errno << " " << strerror(errno);
      } else {
        LOG(INFO) << "Error connecting to " << endpoint << ": " << errno << " "
                  << strerror(errno);
      }
      ::close(sockFd);
      sockFd = -1;
      continue;
    }
  }
  if (sockFd == -1) {
    LOG(ERROR) << "ERROR, no host found";
  } else {
    addToActiveSockets(sockFd);
  }

  freeaddrinfo(results);
  return sockFd;
}

set<int> TcpSocketHandler::listen(const SocketEndpoint &endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  int port = endpoint.getPort();
  if (portServerSockets.find(port) != portServerSockets.end()) {
    LOG(FATAL) << "Tried to listen twice on the same port";
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
    int sockFd;
    if ((sockFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      LOG(INFO) << "Error creating socket " << p->ai_family << "/"
                << p->ai_socktype << "/" << p->ai_protocol << ": " << errno
                << " " << strerror(errno);
      continue;
    }
    initServerSocket(sockFd);

    if (p->ai_family == AF_INET6) {
      // Also ensure that IPV6 sockets only listen on IPV6
      // interfaces.  We will create another socket object for IPV4
      // if it doesn't already exist.
      int flag = 1;
      FATAL_FAIL(setsockopt(sockFd, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&flag,
                            sizeof(int)));
    }

    if (::bind(sockFd, p->ai_addr, p->ai_addrlen) == -1) {
      // This most often happens because the port is in use.
      LOG(ERROR) << "Error binding " << p->ai_family << "/" << p->ai_socktype
                 << "/" << p->ai_protocol << ": " << errno << " "
                 << strerror(errno);
      cerr << "Error binding " << p->ai_family << "/" << p->ai_socktype << "/"
           << p->ai_protocol << ": " << errno << " " << strerror(errno) << endl;
      stringstream oss;
      oss << "Error binding port " << port << ": " << errno << " "
          << strerror(errno);
      string s = oss.str();
      close(sockFd);
      throw std::runtime_error(s.c_str());
    }

    // Listen
    FATAL_FAIL(::listen(sockFd, 32));
    LOG(INFO) << "Listening on "
              << inet_ntoa(((sockaddr_in *)p->ai_addr)->sin_addr) << ":" << port
              << "/" << p->ai_family << "/" << p->ai_socktype << "/"
              << p->ai_protocol;

    // if we get here, we must have connected successfully
    serverSockets.insert(sockFd);
  }

  if (serverSockets.empty()) {
    LOG(FATAL) << "Could not bind to any interface!";
  }

  portServerSockets[port] = serverSockets;
  return serverSockets;
}

set<int> TcpSocketHandler::getEndpointFds(const SocketEndpoint &endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  int port = endpoint.getPort();
  if (portServerSockets.find(port) == portServerSockets.end()) {
    LOG(FATAL)
        << "Tried to getEndpointFds on a port without calling listen() first";
  }
  return portServerSockets[port];
}

void TcpSocketHandler::stopListening(const SocketEndpoint &endpoint) {
  lock_guard<std::recursive_mutex> guard(mutex);

  int port = endpoint.getPort();
  auto it = portServerSockets.find(port);
  if (it == portServerSockets.end()) {
    LOG(FATAL)
        << "Tried to stop listening to a port that we weren't listening on";
  }
  auto &serverSockets = it->second;
  for (int sockFd : serverSockets) {
    close(sockFd);
  }
}

void TcpSocketHandler::initSocket(int fd) {
  UnixSocketHandler::initSocket(fd);
  int flag = 1;
  FATAL_FAIL_UNLESS_EINVAL(
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)));
}
}  // namespace et
