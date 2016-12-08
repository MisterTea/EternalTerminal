#include "UnixSocketHandler.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

namespace et {
UnixSocketHandler::UnixSocketHandler ( ) { }

bool UnixSocketHandler::hasData ( int fd ) {
  fd_set input;
  FD_ZERO ( &input );
  FD_SET ( fd, &input );
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 1 * 100;
  int n = select ( fd + 1, &input, NULL, NULL, &timeout );
  if ( n == -1 ) {
    // Select timed out or failed.
    return false;
  } else if ( n == 0 )
    return false;
  if ( !FD_ISSET ( fd, &input ) ) {
    LOG(FATAL) << "FD_ISSET is false but we should have data by now.";
  }
  return true;
}

ssize_t UnixSocketHandler::read ( int fd, void *buf, size_t count ) {
  ssize_t readBytes = ::read ( fd, buf, count );
  if ( readBytes == 0 ) {
    throw runtime_error ( "Remote host closed connection" );
  }
  return readBytes;
}

ssize_t UnixSocketHandler::write ( int fd, const void *buf, size_t count ) { return ::write ( fd, buf, count ); }

int UnixSocketHandler::connect ( const std::string &hostname, int port ) {
  int sockfd = -1;
  addrinfo* results;
  addrinfo* p;
  addrinfo hints;
  memset(&hints,0,sizeof(addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_CANONNAME;
  std::string portname = std::to_string(port);

  int rc = getaddrinfo ( hostname.c_str ( ), portname.c_str(), &hints, &results);

  if (rc == -1) {
    freeaddrinfo(results);
    LOG(ERROR) << "Error getting address info for " << hostname << ":" << portname << ": "
               << rc << " (" << gai_strerror(rc) << ")";
    return -1;
  }

  // loop through all the results and connect to the first we can
  for(p = results; p != NULL; p = p->ai_next) {
    if ((sockfd = socket(p->ai_family, p->ai_socktype,
                         p->ai_protocol)) == -1) {
      LOG(INFO) << "Error creating socket " << p->ai_canonname << ": " << errno << " " << strerror(errno);
      continue;
    }
    initSocket ( sockfd );

    if (::connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      LOG(INFO) << "Error connecting with " << p->ai_canonname << ": " << errno << " " << strerror(errno);
      close(sockfd);
      sockfd = -1;
      continue;
    }

    LOG(INFO) << "Connected to server: " << p->ai_canonname;
    break; // if we get here, we must have connected successfully
  }

  if (sockfd == -1) {
    LOG ( ERROR ) << "ERROR, no host found";
  }

  return sockfd;
}

int UnixSocketHandler::listen ( int port ) {
  if ( serverSockets.empty() ) {
    addrinfo hints, *servinfo, *p;
    int rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP address

    std::string portname = std::to_string(port);

    if ((rc = getaddrinfo(NULL, portname.c_str(), &hints, &servinfo)) != 0) {
      LOG(ERROR) << "Error getting address info for " << port << ": "
                 << rc << " (" << gai_strerror(rc) << ")";
      exit(1);
    }

    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
      int sockfd;
      if ((sockfd = socket(p->ai_family, p->ai_socktype,
                           p->ai_protocol)) == -1) {
        LOG(INFO) << "Error creating socket " << p->ai_family << "/" << p->ai_socktype << "/" << p->ai_protocol << ": " << errno << " " << strerror(errno);
        continue;
      }
      initSocket(sockfd);
      // Also set the accept socket as non-blocking
      fcntl(sockfd, F_SETFL, O_NONBLOCK);

      if (p->ai_family == AF_INET6) {
        // Also ensure that IPV6 sockets only listen on IPV6
        // interfaces.  We will create another socket object for IPV4
        // if it doesn't already exist.
        int flag = 1;
        FATAL_FAIL ( setsockopt ( sockfd,
                                  IPPROTO_IPV6,
                                  IPV6_V6ONLY,
                                  ( char * ) &flag,
                                  sizeof ( int ) ) );
      }

      if (::bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
        LOG(INFO) << "Error binding " << p->ai_family << "/" << p->ai_socktype << "/" << p->ai_protocol << ": " << errno << " " << strerror(errno);
        close(sockfd);
        continue;
      }

      // Listen
      FATAL_FAIL(::listen ( sockfd, 32 ));
      LOG(INFO) << "Listening on " << p->ai_family << "/" << p->ai_socktype << "/" << p->ai_protocol;

      // if we get here, we must have connected successfully
      serverSockets.push_back(sockfd);
    }

    if (serverSockets.empty()) {
      LOG(FATAL) << "Could not bind to any interface!";
    }

  }

  for (int sockfd : serverSockets) {
    sockaddr_in client;
    socklen_t c = sizeof ( sockaddr_in );
    int client_sock = ::accept ( sockfd, ( sockaddr * ) &client, &c );
    if ( client_sock >= 0 ) {
      initSocket ( client_sock );
      return client_sock;
    } else if(errno != EAGAIN && errno != EWOULDBLOCK) {
      FATAL_FAIL(-1); // LOG(FATAL) with the error
    }
  }

  return -1;
}

void UnixSocketHandler::stopListening ( ) {
  for (int sockfd : serverSockets) {
    close(sockfd);
  }
}

void UnixSocketHandler::close ( int fd ) {
  VLOG ( 1 ) << "Shutting down connection: " << fd << endl;
  int rc = ::shutdown ( fd, SHUT_RDWR );
  if ( rc == -1 ) {
    if ( errno == ENOTCONN ) {
      // This is harmless
    } else {
      FATAL_FAIL ( rc );
    }
  }
  FATAL_FAIL (::close ( fd ) );
}

void UnixSocketHandler::initSocket ( int fd ) {
  int flag = 1;
  FATAL_FAIL ( setsockopt ( fd,                 /* socket affected */
                            IPPROTO_TCP,        /* set option at TCP level */
                            TCP_NODELAY,        /* name of option */
                            ( char * ) &flag,   /* the cast is historical cruft */
                            sizeof ( int ) ) ); /* length of option value */
  timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  FATAL_FAIL ( setsockopt ( fd, SOL_SOCKET, SO_RCVTIMEO, ( char * ) &tv, sizeof ( tv ) ) );
  FATAL_FAIL ( setsockopt ( fd, SOL_SOCKET, SO_SNDTIMEO, ( char * ) &tv, sizeof ( tv ) ) );
}
}
