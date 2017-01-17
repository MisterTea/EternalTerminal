#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "FakeSocketHandler.hpp"

using namespace et;

int main(int argc, char** argv) {
  srand(1);
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  std::shared_ptr<FakeSocketHandler> clientSocket(new FakeSocketHandler());
  std::shared_ptr<FakeSocketHandler> serverSocket(
      new FakeSocketHandler(clientSocket));
  clientSocket->setRemoteHandler(serverSocket);

  serverSocket->addConnection(0);
  serverSocket->listen(0);

  std::array<char, 64 * 1024> s;
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  for (int a = 0; a < 64; a++) {
    clientSocket->write(0, (void*)(&s[0] + a * 1024), 1024);
  }

  std::array<char, 64 * 1024> result;
  serverSocket->read(0, (void*)&result[0], 64 * 1024);

  if (s == result) {
    LOG(INFO) << "Works!";
    return 0;
  }

  std::string sString(s.begin(), s.end());
  std::string resultString(result.begin(), result.end());
  printf("%s != %s", sString.c_str(), resultString.c_str());
  return 1;
}
