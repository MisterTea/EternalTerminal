#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>    /* POSIX Threads */

void* server_main(void*);
void* client_main(void*);

int main() {
  pthread_t thread1, thread2;

  /* create threads 1 and 2 */
  pthread_create (&thread1, NULL, server_main, (void *)NULL);
  pthread_create (&thread2, NULL, client_main, (void *)NULL);

  /* Main block now waits for both threads to terminate, before it exits
     If main block exits, both threads exit, even if the threads have not
     finished their work */
  pthread_join(thread1, NULL);
  pthread_join(thread2, NULL);

  return 0;
}

void client_error(const char *msg)
{
    perror(msg);
    fprintf(stderr, "CLIENT: %s\n",msg);
    fflush(stdout);
    fflush(stderr);
    exit(1);
}

void server_error(const char *msg)
{
    perror(msg);
    fprintf(stderr, "SERVER: %s\n",msg);
    fflush(stdout);
    fflush(stderr);
    exit(1);
}

void* server_main(void*)
{
     int sockfd, newsockfd, portno;
     socklen_t clilen;
     char buffer[256];
     sockaddr_in serv_addr, cli_addr;
     int n;
     sockfd = socket(AF_INET, SOCK_STREAM, 0);
     if (sockfd < 0)
        server_error("ERROR opening socket");
     bzero((char *) &serv_addr, sizeof(serv_addr));
     portno = 11223;
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
     return 0;
}

void* client_main(void*)
{
    int sockfd, portno, n;
    sockaddr_in serv_addr;
    hostent *server;

    char buffer[256];
    portno = 11223;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        client_error("ERROR opening socket");
    server = gethostbyname("localhost");
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host\n");
        exit(0);
    }
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
         (char *)&serv_addr.sin_addr.s_addr,
         server->h_length);
    serv_addr.sin_port = htons(portno);
    if (connect(sockfd,(sockaddr *) &serv_addr,sizeof(serv_addr)) < 0)
        client_error("ERROR connecting");
    bzero(buffer,256);
    sprintf(buffer,"%s","Hello World!");
    n = write(sockfd,buffer,strlen(buffer));
    if (n < 0)
         client_error("ERROR writing to socket");
    bzero(buffer,256);
    n = read(sockfd,buffer,255);
    if (n < 0)
         client_error("ERROR reading from socket");
    printf("%s\n",buffer);
    close(sockfd);
    return 0;
}
