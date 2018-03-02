#include "ClientConnection.hpp"
#include "FakeSocketHandler.hpp"
#include "Headers.hpp"
#include "ServerConnection.hpp"
#include "LogHandler.hpp"

using namespace et;
ServerConnection* globalServer;

void runServer(std::shared_ptr<ServerConnection> server) {
  while (true) {
    server->acceptNewConnection(1);
  }
}

void runClient(std::shared_ptr<FakeSocketHandler> clientSocket,
               std::array<char, 4 * 1024> s) {
  printf("Creating client\n");
  shared_ptr<ClientConnection> client = shared_ptr<ClientConnection>(
      new ClientConnection(clientSocket, "localhost", 1000, "me",
                           "12345678901234567890123456789012"));
  while (true) {
    try {
      client->connect();
    } catch (const runtime_error& err) {
      cout << "Connecting failed, retrying" << endl;
      continue;
    }
    break;
  }
  cout << "Client created with id: " << client->getId() << endl;

  printf("Creating server-client state\n");
  string clientId = client->getId();
  shared_ptr<ServerClientConnection> serverClientState =
      globalServer->getClientConnection(clientId);
  std::array<char, 4 * 1024> result;
  for (int a = 0; a < 4 * 1024; a++) {
    serverClientState->writeMessage(string(&s[0] + a, 1));
    string receivedMessage;
    if (!client->readMessage(&receivedMessage)) {
      LOG(FATAL) << "Error reading message";
    }
    if (receivedMessage.length() != 1) {
      LOG(FATAL) << "Message is the wrong length";
    }
    result[a] = receivedMessage[0];
    cout << "Finished byte " << a << endl;
  }

  if (s == result) {
    cout << "Works!\n";
    exit(0);
  }

  std::string sString(s.begin(), s.end());
  std::string resultString(result.begin(), result.end());
  printf("%s != %s", sString.c_str(), resultString.c_str());
  exit(1);
}

int main(int argc, char** argv) {
  srand(1);
  el::Configurations defaultConf = LogHandler::SetupLogHandler(&argc, &argv);

  defaultConf.setGlobally(el::ConfigurationType::Filename,
                          "testConnection-%datetime.log");
  defaultConf.setGlobally(el::ConfigurationType::ToFile, "true");

  el::Loggers::reconfigureLogger("default", defaultConf);
  std::shared_ptr<FakeSocketHandler> serverSocket(new FakeSocketHandler());
  std::shared_ptr<FakeSocketHandler> clientSocket(
      new FakeSocketHandler(serverSocket));
  serverSocket->setRemoteHandler(clientSocket);

  std::array<char, 4 * 1024> s;
  for (int a = 0; a < 4 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[4 * 1024 - 1] = 0;

  printf("Creating server\n");
  shared_ptr<ServerConnection> server = shared_ptr<ServerConnection>(
      new ServerConnection(serverSocket, 1000, NULL));
  server->addClientKey("me", "12345678901234567890123456789012");
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
