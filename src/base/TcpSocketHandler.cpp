#include "TcpSocketHandler.hpp"

namespace et {
TcpSocketHandler::TcpSocketHandler() {}

int TcpSocketHandler::connect(const SocketEndpoint &endpoint) {
  lock_guard<std::recursive_mutex> guard(globalMutex);
  int sockFd = -1;
  addrinfo *results = NULL;
  addrinfo *p = NULL;
  addrinfo hints;
  memset(&hints, 0, sizeof(addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
#if defined(__NetBSD__) || defined(__ANDROID__)
  hints.ai_flags = (AI_CANONNAME | AI_ADDRCONFIG);
#else
  hints.ai_flags = (AI_CANONNAME | AI_V4MAPPED | AI_ADDRCONFIG | AI_ALL);
#endif
  std::string portname = std::to_string(endpoint.port());
  std::string hostname = endpoint.name();

#ifndef WIN32
  // (re)initialize the DNS system
  ::res_init();
#endif
  int rc = getaddrinfo(hostname.c_str(), portname.c_str(), &hints, &results);

  if (rc == EAI_NONAME) {
    VLOG_EVERY_N(1, 10) << "Cannot resolve hostname: " << gai_strerror(rc);
    if (results) {
      freeaddrinfo(results);
    }
    return -1;
  }

  if (rc != 0) {
    LOG(INFO) << "Error getting address info for " << endpoint << ": " << rc
              << " (" << gai_strerror(rc) << ")";
    if (results) {
      freeaddrinfo(results);
    }
    return -1;
  }

  // loop through all the results and connect to the first we can
  for (p = results; p != NULL; p = p->ai_next) {
    if ((sockFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      auto localErrno = GetErrno();
      LOG(INFO) << "Error creating socket: " << localErrno << " "
                << strerror(localErrno);
      continue;
    }

    // Allow non-blocking connect
    setBlocking(sockFd, false);

    if (::connect(sockFd, p->ai_addr, p->ai_addrlen) == -1 &&
        GetErrno() != EINPROGRESS && GetErrno() != EWOULDBLOCK) {
      auto localErrno = GetErrno();
      if (p->ai_canonname) {
        LOG(INFO) << "Error connecting with " << p->ai_canonname << ": "
                  << localErrno << " " << strerror(localErrno);
      } else {
        LOG(INFO) << "Error connecting: " << localErrno << " "
                  << strerror(localErrno);
      }
      setBlocking(sockFd, true);
#ifdef _MSC_VER
      FATAL_FAIL(::closesocket(sockFd));
#else
      FATAL_FAIL(::close(sockFd));
#endif
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
      VLOG(4) << "sockFd " << sockFd << " is selected";
      int so_error;
      socklen_t len = sizeof so_error;

      FATAL_FAIL(
          ::getsockopt(sockFd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len));

      if (so_error == 0) {
        if (p->ai_canonname) {
          LOG(INFO) << "Connected to server: " << p->ai_canonname
                    << " using fd " << sockFd;
        } else {
          LOG(INFO) << "Connected to server but canonname is null somehow";
        }
        // Make sure that socket becomes blocking once it's attached to a
        // server.
        setBlocking(sockFd, true);
        break;  // if we get here, we must have connected successfully
      } else {
        if (p->ai_canonname) {
          LOG(INFO) << "Error connecting with " << p->ai_canonname << ": "
                    << so_error << " " << strerror(so_error);
        } else {
          LOG(INFO) << "Error connecting to " << endpoint << ": " << so_error
                    << " " << strerror(so_error);
        }
        setBlocking(sockFd, true);
#ifdef _MSC_VER
        FATAL_FAIL(::closesocket(sockFd));
#else
        FATAL_FAIL(::close(sockFd));
#endif
        sockFd = -1;
        continue;
      }
    } else {
      auto localErrno = GetErrno();
      if (p->ai_canonname) {
        LOG(INFO) << "Error connecting with " << p->ai_canonname << ": "
                  << localErrno << " " << strerror(localErrno);
      } else {
        LOG(INFO) << "Error connecting to " << endpoint << ": " << localErrno
                  << " " << strerror(localErrno);
      }
#ifdef _MSC_VER
      FATAL_FAIL(::closesocket(sockFd));
#else
      FATAL_FAIL(::close(sockFd));
#endif
      sockFd = -1;
      continue;
    }
  }
  if (sockFd == -1) {
    LOG(INFO) << "No host found";
  } else {
    initSocket(sockFd);
    addToActiveSockets(sockFd);
  }

  freeaddrinfo(results);
  return sockFd;
}

set<int> TcpSocketHandler::listen(const SocketEndpoint &endpoint) {
  lock_guard<std::recursive_mutex> guard(globalMutex);

  int port = endpoint.port();
  if (portServerSockets.find(port) != portServerSockets.end()) {
    STFATAL << "Tried to listen twice on the same port";
  }

  addrinfo hints, *servinfo, *p;
  int rc;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;  // use any IP address
  const char *bindIp = NULL;
  if (endpoint.has_name()) {
    bindIp = endpoint.name().c_str();
  }

  std::string portname = std::to_string(port);

  if ((rc = getaddrinfo(bindIp, portname.c_str(), &hints, &servinfo)) != 0) {
    STERROR << "Error getting address info for " << port << ": " << rc << " ("
            << gai_strerror(rc) << ")";
    exit(1);
  }

  set<int> serverSockets;
  // loop through all the results and bind to the first we can
  for (p = servinfo; p != NULL; p = p->ai_next) {
    int sockFd;
    if ((sockFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      auto localErrno = GetErrno();
      LOG(INFO) << "Error creating socket " << p->ai_family << "/"
                << p->ai_socktype << "/" << p->ai_protocol << ": " << localErrno
                << " " << strerror(localErrno);
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
      auto localErrno = GetErrno();
      LOG(WARNING) << "Error binding " << p->ai_family << "/" << p->ai_socktype
                   << "/" << p->ai_protocol << ": " << localErrno << " "
                   << strerror(localErrno);
      CLOG(INFO, "stdout") << "Error binding " << p->ai_family << "/"
                           << p->ai_socktype << "/" << p->ai_protocol << ": "
                           << localErrno << " " << strerror(localErrno) << endl;
      stringstream oss;
      oss << "Error binding port " << port << ": " << localErrno << " "
          << strerror(localErrno);
      string s = oss.str();
#ifdef _MSC_VER
      FATAL_FAIL(::closesocket(sockFd));
#else
      FATAL_FAIL(::close(sockFd));
#endif
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
    STFATAL << "Could not bind to any interface!";
  }

  portServerSockets[port] = serverSockets;
  return serverSockets;
}

set<int> TcpSocketHandler::getEndpointFds(const SocketEndpoint &endpoint) {
  lock_guard<std::recursive_mutex> guard(globalMutex);

  int port = endpoint.port();
  if (portServerSockets.find(port) == portServerSockets.end()) {
    STFATAL
        << "Tried to getEndpointFds on a port without calling listen() first";
  }
  return portServerSockets[port];
}

void TcpSocketHandler::stopListening(const SocketEndpoint &endpoint) {
  lock_guard<std::recursive_mutex> guard(globalMutex);

  int port = endpoint.port();
  auto it = portServerSockets.find(port);
  if (it == portServerSockets.end()) {
    STFATAL << "Tried to stop listening to a port that we weren't listening on";
  }
  auto &serverSockets = it->second;
  for (int sockFd : serverSockets) {
#ifdef _MSC_VER
    FATAL_FAIL(::closesocket(sockFd));
#else
    FATAL_FAIL(::close(sockFd));
#endif
  }
  portServerSockets.erase(it);
}

void TcpSocketHandler::initSocket(int fd) {
  UnixSocketHandler::initSocket(fd);
  {
    int flag = 1;
    FATAL_FAIL_UNLESS_EINVAL(
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)));
  }
  {
    // Set linger if possible
    struct linger so_linger;
    so_linger.l_onoff = 1;
    so_linger.l_linger = 5;
    FATAL_FAIL_UNLESS_EINVAL(setsockopt(
        fd, SOL_SOCKET, SO_LINGER, (const char *)&so_linger, sizeof so_linger));
  }
}
}  // namespace et
