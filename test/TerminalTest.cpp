#include "Headers.hpp"

#include "gtest/gtest.h"

#include "FakeConsole.hpp"
#include "Terminal.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"

namespace et {
class FakeConsoleTest : public testing::Test {
 protected:
  void SetUp() override {
    socketHandler.reset(new PipeSocketHandler());
    fakeConsole.reset(new FakeConsole(socketHandler));
    fakeConsole->setup();
  }

  void TearDown() override {
    fakeConsole->teardown();
    fakeConsole.reset();
    socketHandler.reset();
  }

  shared_ptr<PipeSocketHandler> socketHandler;
  shared_ptr<FakeConsole> fakeConsole;
};

TEST_F(FakeConsoleTest, ReadWrite) {
  string s(64 * 1024, '\0');
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  EXPECT_FALSE(socketHandler->hasData(fakeConsole->getFd()));

  thread t([this, s]() { fakeConsole->simulateKeystrokes(s); });
  usleep(1000);

  EXPECT_TRUE(socketHandler->hasData(fakeConsole->getFd()));

  string s2(64 * 1024, '\0');
  socketHandler->readAll(fakeConsole->getFd(), &s2[0], s2.length(), false);

  t.join();

  EXPECT_EQ(s, s2);

  thread t2([this, s]() { fakeConsole->write(s); });

  string s3 = fakeConsole->getTerminalData(s.length());
  EXPECT_EQ(s, s3);

  t2.join();
}

class FakeUserTerminalTest : public testing::Test {
 protected:
  void SetUp() override {
    socketHandler.reset(new PipeSocketHandler());
    fakeUserTerminal.reset(new FakeUserTerminal(socketHandler));
    fakeUserTerminal->setup(-1);
  }

  void TearDown() override {
    fakeUserTerminal->cleanup();
    fakeUserTerminal.reset();
    socketHandler.reset();
  }

  shared_ptr<PipeSocketHandler> socketHandler;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;
};

TEST_F(FakeUserTerminalTest, ReadWrite) {
  string s(64 * 1024, '\0');
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  thread t([this, s]() {
    RawSocketUtils::writeAll(fakeUserTerminal->getFd(), &s[0], s.length());
  });

  string s2 = fakeUserTerminal->getKeystrokes(s.length());
  EXPECT_EQ(s, s2);
  t.join();

  EXPECT_FALSE(socketHandler->hasData(fakeUserTerminal->getFd()));
  thread t2([this, s]() { fakeUserTerminal->simulateTerminalResponse(s); });
  usleep(1000);
  EXPECT_TRUE(socketHandler->hasData(fakeUserTerminal->getFd()));

  string s3(64 * 1024, '\0');
  socketHandler->readAll(fakeUserTerminal->getFd(), &s3[0], s3.length(), false);

  t2.join();
  EXPECT_EQ(s, s3);
}

class EndToEndTest : public testing::Test {
 protected:
  void SetUp() override {
    el::Helpers::setThreadName("Main");
    consoleSocketHandler.reset(new PipeSocketHandler());
    fakeConsole.reset(new FakeConsole(consoleSocketHandler));
    fakeConsole->setup();

    userTerminalSocketHandler.reset(new PipeSocketHandler());
    fakeUserTerminal.reset(new FakeUserTerminal(userTerminalSocketHandler));
    fakeUserTerminal->setup(-1);

    string tmpPath = string("/tmp/etserver_test_XXXXXXXX");
    pipeDirectory = string(mkdtemp(&tmpPath[0]));
    pipePath = string(pipeDirectory) + "/pipe";
    serverEndpoint = SocketEndpoint(pipePath);

    thread t_server([this]() {
      et::startServer(serverSocketHandler, serverEndpoint, routerSocketHandler);
    });
  }

  void TearDown() override {
    t_server.join();

    consoleSocketHandler.reset();
    userTerminalSocketHandler.reset();
    serverSocketHandler.reset();
    clientSocketHandler.reset();
    routerSocketHandler.reset();
    FATAL_FAIL(::remove(pipePath.c_str()));
    FATAL_FAIL(::remove(pipeDirectory.c_str()));
  }

  void readWriteTest(const string& clientId) {
    thread t_terminal([this, clientId]() {
      et::startUserTerminal(routerSocketHandler, fakeUserTerminal,
                            clientId + CRYPTO_KEY, true);
    });

    shared_ptr<TerminalClient> terminalClient(
        new TerminalClient(clientSocketHandler, serverEndpoint, clientId,
                           CRYPTO_KEY, fakeConsole));

    const int NUM_MESSAGES = 32;
    string s(NUM_MESSAGES * 1024, '\0');
    for (int a = 0; a < NUM_MESSAGES * 1024; a++) {
      s[a] = rand() % 26 + 'A';
    }

    for (int a = 0; a < NUM_MESSAGES; a++) {
      VLOG(1) << "Writing packet " << a;
      fakeConsole->simulateKeystrokes(string((&s[0] + a * 1024)));
    }

    string resultConcat;
    string result;
    for (int a = 0; a < NUM_MESSAGES; a++) {
      result = fakeUserTerminal->getKeystrokes(1024);
      resultConcat = resultConcat.append(result);
      LOG(INFO) << "ON MESSAGE " << a;
    }

    EXPECT_EQ(resultConcat, s);

    // TODO: reverse order
  }
  shared_ptr<PipeSocketHandler> consoleSocketHandler;
  shared_ptr<PipeSocketHandler> userTerminalSocketHandler;
  shared_ptr<PipeSocketHandler> routerSocketHandler;

  shared_ptr<SocketHandler> serverSocketHandler;
  shared_ptr<SocketHandler> clientSocketHandler;

  SocketEndpoint serverEndpoint;

  shared_ptr<FakeConsole> fakeConsole;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;

  string pipeDirectory;
  string pipePath;
  thread t_server;

  const string CRYPTO_KEY = "12345678901234567890123456789012";
};

class ReliableEndToEndTest : public EndToEndTest {
 protected:
  void SetUp() override {
    srand(1);
    clientSocketHandler.reset(new PipeSocketHandler());
    serverSocketHandler.reset(new PipeSocketHandler());
    routerSocketHandler.reset(new PipeSocketHandler());

    EndToEndTest::SetUp();
  }
};

TEST_F(ReliableEndToEndTest, ReadWrite) { readWriteTest("1234567890123456"); }

// TODO: Multiple clients

// TODO: FlakySocket
}  // namespace et