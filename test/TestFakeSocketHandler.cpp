#include "Headers.hpp"

#include "gtest/gtest.h"

#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "FakeSocketHandler.hpp"
#include "LogHandler.hpp"

using namespace et;

class FakeSocketHandlerTest : public testing::Test {
 protected:
  virtual void SetUp() {
    srand(1);

    serverSocketHandler.reset(new FakeSocketHandler());
    clientSocketHandler.reset(
        new FakeSocketHandler(serverSocketHandler));
    serverSocketHandler->setRemoteHandler(clientSocketHandler);

    fd = serverSocketHandler->fakeConnection();
  }

  shared_ptr<FakeSocketHandler> serverSocketHandler;
  shared_ptr<FakeSocketHandler> clientSocketHandler;
  int fd;
};

TEST_F(FakeSocketHandlerTest, ReadWrite) {
  srand(1);

  std::array<char, 64 * 1024> s;
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  for (int a = 0; a < 64; a++) {
    clientSocketHandler->write(fd, (void*)(&s[0] + a * 1024), 1024);
  }

  std::array<char, 64 * 1024> result;
  serverSocketHandler->read(fd, (void*)&result[0], 64 * 1024);

  EXPECT_EQ(result, s);
}
