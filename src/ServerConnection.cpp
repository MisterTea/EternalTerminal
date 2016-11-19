#include "ServerConnection.hpp"

void server_error(const char *msg)
{
    perror(msg);
    fprintf(stderr, "SERVER: %s\n",msg);
    fflush(stdout);
    fflush(stderr);
    exit(1);
}

ServerConnection::ServerConnection(int port) {
  this->port = port;
  this->finish = false;
}

void ServerConnection::run() {
  int sockfd, newsockfd, portno;
  socklen_t clilen;
  char buffer[256];
  sockaddr_in serv_addr, cli_addr;
  int n;
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0)
    server_error("ERROR opening socket");
  bzero((char *) &serv_addr, sizeof(serv_addr));
  portno = this->port;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(portno);
  if (bind(sockfd, (sockaddr *) &serv_addr,
           sizeof(serv_addr)) < 0)
    server_error("ERROR on binding");
  listen(sockfd,5);
  clilen = sizeof(cli_addr);
  newsockfd = accept(sockfd,
                     (sockaddr *) &cli_addr,
                     &clilen);
  if (newsockfd < 0)
    server_error("ERROR on accept");
  bzero(buffer,256);
  n = read(newsockfd,buffer,255);
  if (n < 0) server_error("ERROR reading from socket");
  printf("Here is the message: %s\n",buffer);
  if (strcmp(buffer,"Hello World!")) {
    server_error("Data is corrupt");
  }
  n = write(newsockfd,"I got your message",18);
  if (n < 0) server_error("ERROR writing to socket");
  close(newsockfd);
  close(sockfd);
}
