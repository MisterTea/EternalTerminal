#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "FakeSocketHandler.hpp"

int main() {
  std::shared_ptr<FakeSocketHandler> serverSocket(new FakeSocketHandler());
  std::shared_ptr<FakeSocketHandler> clientSocket(new FakeSocketHandler(serverSocket));
  serverSocket->setRemoteHandler(clientSocket);

  BackedReader serverReader(serverSocket, 0);
  BackedWriter serverWriter(serverSocket, 0);

  BackedReader clientReader(clientSocket, 0);
  BackedWriter clientWriter(clientSocket, 0);

  std::array<char,64*1024> s;
  for (int a=0;a<64*1024 - 1;a++) {
    s[a] = rand()%26 + 'A';
  }
  s[64*1024 - 1] = 0;

  for (int a=0;a<64;a++) {
    serverWriter.write((void*)(&s[0] + a*1024), 1024);
  }

  std::array<char,64*1024> result;
  clientReader.read((void*)&result[0], 64*1024);

  if (s == result) {
    return 0;
  }

  std::string sString(s.begin(), s.end());
  std::string resultString(result.begin(), result.end());
  printf("%s != %s",sString.c_str(),resultString.c_str());
  return 1;
}
