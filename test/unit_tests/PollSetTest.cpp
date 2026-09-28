#include "PollSet.hpp"
#include "RawSocketUtils.hpp"
#include "TestHeaders.hpp"
#include "TestSocketPair.hpp"

using namespace et;
using namespace et::test;

TEST_CASE("PollSet reports a readable socket", "[PollSet]") {
  int sockets[2];
  REQUIRE(createTestSocketPair(sockets) == 0);

  {
    PollSet idle;
    idle.watch(sockets[0], true, false);
    CHECK(idle.waitReadable(0).empty());
  }

  const char byte = 'x';
  REQUIRE(RawSocketUtils::writeSome(sockets[1], &byte, 1) == 1);
  PollSet pollSet;
  pollSet.watch(sockets[0], true, false);
  const set<int> ready = pollSet.waitReadable(1000);
  CHECK(ready == set<int>{sockets[0]});

  closeTestFd(sockets[0]);
  closeTestFd(sockets[1]);
}

TEST_CASE("PollSet only reports fds watched for read", "[PollSet]") {
  int sockets[2];
  REQUIRE(createTestSocketPair(sockets) == 0);
  const char byte = 'x';
  REQUIRE(RawSocketUtils::writeSome(sockets[1], &byte, 1) == 1);

  {
    PollSet writeOnly;
    writeOnly.watch(sockets[0], false, true);
    CHECK(writeOnly.waitReadable(100).empty());
  }
  {
    PollSet merged;
    merged.watch(sockets[0], false, true);
    merged.watch(sockets[0], true, false);
    CHECK(merged.waitReadable(1000) == set<int>{sockets[0]});
  }

  closeTestFd(sockets[0]);
  closeTestFd(sockets[1]);
}

TEST_CASE("PollSet reports a hung-up peer as readable", "[PollSet]") {
  int sockets[2];
  REQUIRE(createTestSocketPair(sockets) == 0);
  closeTestFd(sockets[1]);

  PollSet pollSet;
  pollSet.watch(sockets[0], true, false);
  CHECK(pollSet.waitReadable(1000) == set<int>{sockets[0]});

  closeTestFd(sockets[0]);
}

TEST_CASE("PollSet ignores negative fds and sleeps when empty", "[PollSet]") {
  PollSet pollSet;
  pollSet.watch(-1, true, true);
  const auto start = std::chrono::steady_clock::now();
  CHECK(pollSet.waitReadable(50).empty());
  CHECK(std::chrono::steady_clock::now() - start >=
        std::chrono::milliseconds(40));
}
