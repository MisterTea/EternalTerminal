#include "Headers.hpp"
#include "ClientConnection.hpp"
#include "ServerConnection.hpp"
#include "FlakyFakeSocketHandler.hpp"

using namespace et;
ServerConnection *globalServer;

void runServer(
  std::shared_ptr<ServerConnection> server) {
  server->run();
}

void runClient(
  std::shared_ptr<FlakyFakeSocketHandler> clientSocket,
  std::array<char,4*1024> s
  ) {
  printf("Creating client\n");
  shared_ptr<ClientConnection> client = shared_ptr<ClientConnection>(
    new ClientConnection(clientSocket, "localhost", 1000, "12345678901234567890123456789012"));
  while(true) {
    try {
      client->connect();
    } catch (const runtime_error& err) {
      cout << "Connecting failed, retrying" << endl;
      continue;
    }
    break;
  }
  cout << "Client created with id: " << client->getClientId() << endl;

  printf("Creating server-client state\n");
  int clientId = client->getClientId();
  shared_ptr<ServerClientConnection> serverClientState = globalServer->getClient(clientId);
  std::array<char,4*1024> result;
  for (int a=0;a<4*1024;a++) {
    serverClientState->writeAll((void*)(&s[0] + a), 1);
    client->readAll((void*)(&result[0] + a), 1);
    cout << "Finished byte " << a << endl;
  }

  if (s == result) {
    cout << "Works!\n";
    exit(0);
  }

  std::string sString(s.begin(), s.end());
  std::string resultString(result.begin(), result.end());
  printf("%s != %s",sString.c_str(),resultString.c_str());
  exit(1);
}

int main(int argc, char** argv) {
  srand(1);
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  std::shared_ptr<FakeSocketHandler> serverSocket(new FakeSocketHandler());
  std::shared_ptr<FlakyFakeSocketHandler> clientSocket(new FlakyFakeSocketHandler(serverSocket, 5000));
  serverSocket->setRemoteHandler(clientSocket);

  std::array<char,4*1024> s;
  for (int a=0;a<4*1024 - 1;a++) {
    s[a] = rand()%26 + 'A';
  }
  s[4*1024 - 1] = 0;

  printf("Creating server\n");
  shared_ptr<ServerConnection> server = shared_ptr<ServerConnection>(
    new ServerConnection(serverSocket, 1000, NULL, "12345678901234567890123456789012"));
  globalServer = server.get();

  thread serverThread(runServer, server);
  thread clientThread(runClient, clientSocket, s);
  printf("Init complete!\n");


  clientThread.join();
  cout << "CLOSING SERVER\n";
  server->close();
  serverThread.join();
  return 0;
}
