#include "UnixSocketHandler.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

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

void UnixSocketHandler::close(int fd) {
  ::close(fd);
}
