#include "UnixSocketHandler.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

UnixSocketHandler::UnixSocketHandler() {
  serverSocket = -1;
}

bool UnixSocketHandler::hasData(int fd) {
  fd_set input;
  FD_ZERO(&input);
  FD_SET(fd, &input);
  struct timeval timeout;
  timeout.tv_sec  = 0;
  timeout.tv_usec = 1 * 100;
  int n = select(fd + 1, &input, NULL, NULL, &timeout);
  if (n == -1) {
    //something wrong
    throw runtime_error("Oops");
  } else if (n == 0)
    return false;
  if (!FD_ISSET(fd, &input)) {
    throw runtime_error("Oops");
  }
  return true;
}

ssize_t UnixSocketHandler::read(int fd, void* buf, size_t count) {
  ssize_t readBytes = ::read(fd, buf, count);
  if (readBytes == 0) {
    throw runtime_error("Remote host closed connection");
  }
  return readBytes;
}

ssize_t UnixSocketHandler::write(int fd, const void* buf, size_t count) {
  return ::write(fd, buf, count);
}

int UnixSocketHandler::connect(const std::string &hostname, int port) {
  int sockfd;
  sockaddr_in serv_addr;
  hostent *server;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    LOG(ERROR) << "ERROR opening socket";
    return -1;
  }
  initSocket(sockfd);
  server = gethostbyname(hostname.c_str());
  if (server == NULL) {
    LOG(ERROR) << "ERROR, no such host";
    return -1;
  }
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  bcopy((char *)server->h_addr,
        (char *)&serv_addr.sin_addr.s_addr,
        server->h_length);
  serv_addr.sin_port = htons(port);
  if (::connect(sockfd,(sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
    LOG(ERROR) << "ERROR connecting to " << hostname << ":" << port;
    ::close(sockfd);
    return -1;
  }
  return sockfd;
}

int UnixSocketHandler::listen(int port) {
  if (serverSocket == -1) {
    // Initialize server socket
    struct sockaddr_in server;

    //Create socket
    int socket_desc = socket(AF_INET , SOCK_STREAM , 0);
    initSocket(socket_desc);
    if (socket_desc == -1)
    {
      throw std::runtime_error("Could not create socket");
    }

    //Prepare the sockaddr_in structure
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons( port );

    //Bind
    if( ::bind(socket_desc,(struct sockaddr *)&server , sizeof(server)) < 0)
    {
      throw std::runtime_error("Bind Failed");
    }

    //Listen
    ::listen(socket_desc , 3);
    serverSocket = socket_desc;
  }

  sockaddr_in client;
  socklen_t c = sizeof(sockaddr_in);
  int client_sock = ::accept(serverSocket, (sockaddr *)&client, &c);
  if (client_sock >= 0) {
    initSocket(client_sock);
  }
  return client_sock;
}

void UnixSocketHandler::stopListening() {
  close(serverSocket);
}

void UnixSocketHandler::close(int fd) {
  ::close(fd);
}

void UnixSocketHandler::initSocket(int fd) {
  int flag=1;
  int result = setsockopt(fd,            /* socket affected */
                          IPPROTO_TCP,     /* set option at TCP level */
                          TCP_NODELAY,     /* name of option */
                          (char *) &flag,  /* the cast is historical
                                              cruft */
                          sizeof(int));    /* length of option value */
  FATAL_FAIL(result);
}
