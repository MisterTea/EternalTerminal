#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "FakeSocketHandler.hpp"

int main(int argc, char** argv) {
  srand(1);
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  std::shared_ptr<FakeSocketHandler> serverSocket(new FakeSocketHandler());
  std::shared_ptr<FakeSocketHandler> clientSocket(new FakeSocketHandler(serverSocket));
  serverSocket->setRemoteHandler(clientSocket);

  BackedReader serverReader(serverSocket, 0);
  BackedWriter serverWriter(serverSocket, 0);
  serverSocket->addConnection(0);
  serverSocket->listen(0);

  BackedReader clientReader(clientSocket, 0);
  BackedWriter clientWriter(clientSocket, 0);
  clientSocket->addConnection(0);
  clientSocket->listen(0);

  std::array<char,64*1024> s;
  for (int a=0;a<64*1024 - 1;a++) {
    s[a] = rand()%26 + 'A';
  }
  s[64*1024 - 1] = 0;

  for (int a=0;a<64;a++) {
    int r = serverWriter.write((void*)(&s[0] + a*1024), 1024);
    if (r != 1024) {
      throw runtime_error("Oops");
    }
  }

  std::array<char,64*1024> result;
  int c = clientReader.read((void*)&result[0], 64*1024);
  if (c != 64*1024) {
    throw runtime_error("Oops");
  }

  if (s == result) {
    return 0;
  }

  std::string sString(s.begin(), s.end());
  std::string resultString(result.begin(), result.end());
  printf("%s != %s",sString.c_str(),resultString.c_str());
  return 1;
}
