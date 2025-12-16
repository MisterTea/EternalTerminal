#include <atomic>

#include "FakeConsole.hpp"
#include "TerminalClient.hpp"
#include "TerminalServer.hpp"
#include "TestHeaders.hpp"
#include "TunnelUtils.hpp"

namespace et {
TEST_CASE("FakeConsoleTest", "[FakeConsoleTest]") {
  shared_ptr<PipeSocketHandler> socketHandler;
  shared_ptr<FakeConsole> fakeConsole;
  socketHandler.reset(new PipeSocketHandler());
  fakeConsole.reset(new FakeConsole(socketHandler));
  fakeConsole->setup();

  string s(64 * 1024, '\0');
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  REQUIRE(!socketHandler->hasData(fakeConsole->getFd()));

  thread t([fakeConsole, s]() { fakeConsole->simulateKeystrokes(s); });
  sleep(1);

  REQUIRE(socketHandler->hasData(fakeConsole->getFd()));

  string s2(64 * 1024, '\0');
  socketHandler->readAll(fakeConsole->getFd(), &s2[0], s2.length(), false);

  t.join();

  REQUIRE(s == s2);

  thread t2([fakeConsole, s]() { fakeConsole->write(s); });

  string s3 = fakeConsole->getTerminalData(s.length());
  REQUIRE(s == s3);

  t2.join();

  fakeConsole->teardown();
  fakeConsole.reset();
  socketHandler.reset();
}

TEST_CASE("FakeUserTerminalTest", "[FakeUserTerminalTest]") {
  shared_ptr<PipeSocketHandler> socketHandler;
  shared_ptr<FakeUserTerminal> fakeUserTerminal;
  socketHandler.reset(new PipeSocketHandler());
  fakeUserTerminal.reset(new FakeUserTerminal(socketHandler));
  fakeUserTerminal->setup(-1);

  string s(64 * 1024, '\0');
  for (int a = 0; a < 64 * 1024 - 1; a++) {
    s[a] = rand() % 26 + 'A';
  }
  s[64 * 1024 - 1] = 0;

  thread t([fakeUserTerminal, s]() {
    RawSocketUtils::writeAll(fakeUserTerminal->getFd(), &s[0], s.length());
  });

  string s2 = fakeUserTerminal->getKeystrokes(s.length());
  REQUIRE(s == s2);
  t.join();

  REQUIRE(!socketHandler->hasData(fakeUserTerminal->getFd()));
  thread t2([fakeUserTerminal, s]() {
    fakeUserTerminal->simulateTerminalResponse(s);
  });

  string s3(64 * 1024, '\0');
  socketHandler->readAll(fakeUserTerminal->getFd(), &s3[0], s3.length(), false);

  t2.join();
  REQUIRE(s == s3);

  fakeUserTerminal->cleanup();
  fakeUserTerminal.reset();
  socketHandler.reset();
}

}  // namespace et
