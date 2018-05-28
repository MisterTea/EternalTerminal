#include "Headers.hpp"

#include "gtest/gtest.h"

#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "CryptoHandler.hpp"
#include "FakeSocketHandler.hpp"
#include "LogHandler.hpp"

using namespace et;

class BackedTest : public testing::Test {
 protected:
  virtual void SetUp() {
    srand(1);

    serverSocketHandler.reset(new FakeSocketHandler());
    clientSocketHandler.reset(
        new FakeSocketHandler(serverSocketHandler));
    serverSocketHandler->setRemoteHandler(clientSocketHandler);

    int fd = serverSocketHandler->fakeConnection();

    serverReader.reset(new BackedReader(
        serverSocketHandler,
        shared_ptr<CryptoHandler>(new CryptoHandler(
            "12345678901234567890123456789012", CLIENT_SERVER_NONCE_MSB)),
        fd));
    serverWriter.reset(new BackedWriter(
        serverSocketHandler,
        shared_ptr<CryptoHandler>(new CryptoHandler(
            "12345678901234567890123456789012", SERVER_CLIENT_NONCE_MSB)),
        fd));

    clientReader.reset(new BackedReader(
        clientSocketHandler,
        shared_ptr<CryptoHandler>(new CryptoHandler(
            "12345678901234567890123456789012", SERVER_CLIENT_NONCE_MSB)),
        fd));
    clientWriter.reset(new BackedWriter(
        clientSocketHandler,
        shared_ptr<CryptoHandler>(new CryptoHandler(
            "12345678901234567890123456789012", CLIENT_SERVER_NONCE_MSB)),
        fd));
  }

  shared_ptr<FakeSocketHandler> serverSocketHandler;
  shared_ptr<FakeSocketHandler> clientSocketHandler;
  shared_ptr<BackedReader> serverReader;
  shared_ptr<BackedWriter> serverWriter;
  shared_ptr<BackedReader> clientReader;
  shared_ptr<BackedWriter> clientWriter;
};

TEST_F(BackedTest, ReadWrite) {
  string s(64 * 1024, '\0');
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  for (int a = 0; a < 64; a++) {
    BackedWriterWriteState r =
        serverWriter->write(string((&s[0] + a * 1024), 1024));
    if (r != BackedWriterWriteState::SUCCESS) {
      throw runtime_error("Oops");
    }
  }

  string resultConcat;
  string result;
  for (int a = 0; a < 64; a++) {
    int c = clientReader->read(&result);
    EXPECT_EQ(c, 1);
    resultConcat = resultConcat.append(result);
  }
  EXPECT_EQ(resultConcat, s);
}
