#include "Console.hpp"
#include "RawSocketUtils.hpp"
#include "TestHeaders.hpp"
#include "TestSocketPair.hpp"

using namespace et;
using namespace et::test;

namespace {
// Exercises the default Console I/O on a socket, as FakeConsole does.
class SocketConsole : public Console {
 public:
  explicit SocketConsole(int _fd) : fd(_fd) {}
  std::optional<TerminalInfo> getTerminalInfo() override {
    return std::nullopt;
  }
  void setup() override {}
  void teardown() override {}
  int getFd() override { return fd; }

 private:
  int fd;
};
}  // namespace

TEST_CASE("Console reads socket input until the peer closes", "[Console]") {
  int sockets[2];
  REQUIRE(createTestSocketPair(sockets) == 0);
  REQUIRE(setSocketBlocking(sockets[0], false));
  SocketConsole console(sockets[0]);

  CHECK(console.getInputPollFds() == vector<int>{sockets[0]});
  CHECK(console.getOutputPollFd() == sockets[0]);

  string input;
  CHECK(console.readInput({}, &input) == ConsoleInputStatus::NONE);
  CHECK(input.empty());

  const string keys = "ls\r";
  REQUIRE(RawSocketUtils::writeSome(sockets[1], keys.data(), keys.size()) ==
          static_cast<ssize_t>(keys.size()));
  REQUIRE(waitOnSocketData(sockets[0], 1, 0));
  CHECK(console.readInput({sockets[0]}, &input) == ConsoleInputStatus::DATA);
  CHECK(input == keys);

  closeTestFd(sockets[1]);
  REQUIRE(waitOnSocketData(sockets[0], 1, 0));
  string eof;
  CHECK(console.readInput({sockets[0]}, &eof) == ConsoleInputStatus::CLOSED);
  CHECK(eof.empty());

  closeTestFd(sockets[0]);
}

TEST_CASE("Console writeSome delivers bytes to the fd", "[Console]") {
#ifdef WIN32
  SKIP("The default Windows Console writes to the process stdout handle");
#else
  int sockets[2];
  REQUIRE(createTestSocketPair(sockets) == 0);
  SocketConsole console(sockets[0]);

  CHECK(console.writeSome("") == 0);
  CHECK(console.writeSome("hello") == 5);
  console.write(" world");
  string received(11, '\0');
  RawSocketUtils::readAll(sockets[1], &received[0], received.size());
  CHECK(received == "hello world");

  closeTestFd(sockets[0]);
  closeTestFd(sockets[1]);
#endif
}
