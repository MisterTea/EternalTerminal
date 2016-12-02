#include "Headers.hpp"
#include "ClientConnection.hpp"
#include "ServerConnection.hpp"
#include "FakeSocketHandler.hpp"

ServerConnection *globalServer;

void runServer(
  std::shared_ptr<ServerConnection> server) {
  server->run();
}

void runClient(
  std::shared_ptr<FakeSocketHandler> clientSocket,
  std::array<char,64*1024> s
  ) {
  printf("Creating client\n");
  ClientConnection client(clientSocket, "localhost", 1000);
  printf("Client created!\n");

  printf("Creating server-client state\n");
  int clientId = *(globalServer->getClientIds().begin());
  shared_ptr<ServerClientConnection> serverClientState = globalServer->getClient(clientId);
  for (int a=0;a<64;a++) {
    serverClientState->write((void*)(&s[0] + a*1024), 1024);
  }

  std::array<char,64*1024> result;
  client.read((void*)&result[0], 64*1024);

  if (s == result) {
    cout << "Works!\n";
    exit(0);
  }

  std::string sString(s.begin(), s.end());
  std::string resultString(result.begin(), result.end());
  printf("%s != %s",sString.c_str(),resultString.c_str());
  exit(1);
}

int main() {
  std::shared_ptr<FakeSocketHandler> serverSocket(new FakeSocketHandler());
  std::shared_ptr<FakeSocketHandler> clientSocket(new FakeSocketHandler(serverSocket));
  serverSocket->setRemoteHandler(clientSocket);

  std::array<char,64*1024> s;
  for (int a=0;a<64*1024 - 1;a++) {
    s[a] = rand()%26 + 'A';
  }
  s[64*1024 - 1] = 0;

  printf("Creating server\n");
  shared_ptr<ServerConnection> server = shared_ptr<ServerConnection>(
    new ServerConnection(serverSocket, 1000));
  globalServer = server.get();
  int nullId = -1;
  clientSocket->write(-1,&nullId,sizeof(int));
  thread serverThread(runServer, server);

  std::thread t1(runClient, clientSocket, s);
  printf("Init complete!\n");
  // TODO: The server needs to be shut down at some point


  t1.join();
  return 0;
}
