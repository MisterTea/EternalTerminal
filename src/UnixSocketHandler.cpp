#include "UnixSocketHandler.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
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
  timeout.tv_usec = 1 * 1000;
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
  return ::read(fd, buf, count);
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
    fprintf(stderr, "ERROR opening socket\n");
    return -1;
  }
  server = gethostbyname(hostname.c_str());
  if (server == NULL) {
    fprintf(stderr, "ERROR, no such host\n");
    return -1;
  }
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  bcopy((char *)server->h_addr,
        (char *)&serv_addr.sin_addr.s_addr,
        server->h_length);
  serv_addr.sin_port = htons(port);
  if (::connect(sockfd,(sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
    fprintf(stderr, "ERROR connecting\n");
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
    if (socket_desc == -1)
    {
      throw new std::runtime_error("Could not create socket");
    }

    //Prepare the sockaddr_in structure
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons( port );

    //Bind
    if( ::bind(socket_desc,(struct sockaddr *)&server , sizeof(server)) < 0)
    {
      throw new std::runtime_error("Bind Failed");
    }

    //Listen
    ::listen(socket_desc , 3);
    serverSocket = socket_desc;
  }

  sockaddr_in client;
  socklen_t c = sizeof(sockaddr_in);
  int client_sock = ::accept(serverSocket, (sockaddr *)&client, &c);
  return client_sock;
}

void UnixSocketHandler::close(int fd) {
  ::close(fd);
}
