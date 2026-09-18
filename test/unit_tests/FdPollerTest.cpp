#include "FdPoller.hpp"
#include "TcpSocketHandler.hpp"
#include "TestHeaders.hpp"
#include "TestSocketPair.hpp"

using namespace et;
using namespace et::test;

namespace {
// Writes the whole buffer to a test socket (blocking).
inline void writeTestSocket(int fd, const void* buf, size_t count) {
#ifdef WIN32
  REQUIRE(::send(fd, static_cast<const char*>(buf), static_cast<int>(count),
                 0) == static_cast<int>(count));
#else
  REQUIRE(::write(fd, buf, count) == static_cast<ssize_t>(count));
#endif
}
}  // namespace

TEST_CASE("FdPoller supports descriptors above FD_SETSIZE", "[FdPoller]") {
#ifdef WIN32
  // WSAPoll (like epoll/kqueue) has no FD_SETSIZE ceiling: any socket proves
  // the poller does not depend on fd_set bit positions.
  int sockets[2];
  REQUIRE(createTestSocketPair(sockets) == 0);

  FdPoller poller;
  poller.setFds({sockets[0]});

  char byte = 'x';
  CHECK(isSocketWritable(sockets[1]));
  writeTestSocket(sockets[1], &byte, 1);
  CHECK(waitOnSocketData(sockets[0]));
  TcpSocketHandler socketHandler;
  CHECK(socketHandler.waitForData(sockets[0], 0, 0));
  CHECK(poller.wait(1000).readable.count(sockets[0]) == 1);

  test::closeTestFd(sockets[0]);
  test::closeTestFd(sockets[1]);
#else
  int pipeFds[2];
  REQUIRE(pipe(pipeFds) == 0);
  int highFd = fcntl(pipeFds[0], F_DUPFD, FD_SETSIZE);
  REQUIRE(highFd >= FD_SETSIZE);
  REQUIRE(close(pipeFds[0]) == 0);
  int highWriteFd = fcntl(pipeFds[1], F_DUPFD, FD_SETSIZE);
  REQUIRE(highWriteFd >= FD_SETSIZE);
  REQUIRE(close(pipeFds[1]) == 0);

  FdPoller poller;
  poller.setFds({highFd});

  char byte = 'x';
  CHECK(isSocketWritable(highWriteFd));
  REQUIRE(write(highWriteFd, &byte, 1) == 1);
  CHECK(waitOnSocketData(highFd));
  TcpSocketHandler socketHandler;
  CHECK(socketHandler.waitForData(highFd, 0, 0));
  CHECK(poller.wait(1000).readable.count(highFd) == 1);

  CHECK(close(highFd) == 0);
  CHECK(close(highWriteFd) == 0);
#endif
}

TEST_CASE("FdPoller replaces its descriptor set", "[FdPoller]") {
  int firstPair[2];
  int secondPair[2];
  REQUIRE(createTestSocketPair(firstPair) == 0);
  REQUIRE(createTestSocketPair(secondPair) == 0);

  FdPoller poller;
  poller.setFds({firstPair[0]});
  CHECK(poller.wait(0).readable.empty());

  poller.setFds({secondPair[0]});
  char byte = 'x';
  writeTestSocket(secondPair[1], &byte, 1);
  FdPoller::Ready ready = poller.wait(1000);
  CHECK(ready.readable.count(firstPair[0]) == 0);
  CHECK(ready.readable.count(secondPair[0]) == 1);

  test::closeTestFd(firstPair[0]);
  test::closeTestFd(firstPair[1]);
  test::closeTestFd(secondPair[0]);
  test::closeTestFd(secondPair[1]);
}

TEST_CASE("FdPoller reports write readiness", "[FdPoller]") {
  int sockets[2];
  REQUIRE(createTestSocketPair(sockets) == 0);

  FdPoller poller;
  poller.setFds({sockets[0]}, {sockets[1]});

  FdPoller::Ready ready = poller.wait(1000);
  // An empty socket is writable but not readable.
  CHECK(ready.writable.count(sockets[1]) == 1);
  CHECK(ready.readable.count(sockets[0]) == 0);

  char byte = 'x';
  writeTestSocket(sockets[1], &byte, 1);
  ready = poller.wait(1000);
  CHECK(ready.readable.count(sockets[0]) == 1);

  test::closeTestFd(sockets[0]);
  test::closeTestFd(sockets[1]);
}

TEST_CASE("FdPoller refreshes a reused descriptor number", "[FdPoller]") {
#ifdef WIN32
  // Windows SOCKET values come from the kernel handle table and cannot be
  // dup2'ed onto each other like Unix fds. Exercise the same refresh path by
  // closing a watched socket and watching a new one: the poller must report
  // the new socket readable rather than stale state for the old number.
  int firstPair[2];
  int secondPair[2];
  REQUIRE(createTestSocketPair(firstPair) == 0);
  REQUIRE(createTestSocketPair(secondPair) == 0);

  FdPoller poller;
  poller.setFds({firstPair[0]});

  test::closeTestFd(firstPair[0]);
  test::closeTestFd(firstPair[1]);
  poller.setFds({secondPair[0]}, {}, {secondPair[0]});

  char byte = 'x';
  writeTestSocket(secondPair[1], &byte, 1);
  CHECK(poller.wait(1000).readable.count(secondPair[0]) == 1);

  test::closeTestFd(secondPair[0]);
  test::closeTestFd(secondPair[1]);
#else
  int firstPipe[2];
  int secondPipe[2];
  REQUIRE(pipe(firstPipe) == 0);
  REQUIRE(pipe(secondPipe) == 0);

  int watchedFd = firstPipe[0];
  FdPoller poller;
  poller.setFds({watchedFd});

  REQUIRE(close(watchedFd) == 0);
  REQUIRE(dup2(secondPipe[0], watchedFd) == watchedFd);
  REQUIRE(close(secondPipe[0]) == 0);
  poller.setFds({watchedFd}, {}, {watchedFd});

  char byte = 'x';
  REQUIRE(write(secondPipe[1], &byte, 1) == 1);
  CHECK(poller.wait(1000).readable.count(watchedFd) == 1);

  CHECK(close(watchedFd) == 0);
  CHECK(close(firstPipe[1]) == 0);
  CHECK(close(secondPipe[1]) == 0);
#endif
}

TEST_CASE("Socket polling surfaces invalid descriptors", "[FdPoller]") {
  int sockets[2];
  REQUIRE(createTestSocketPair(sockets) == 0);
  test::closeTestFd(sockets[0]);

  CHECK(waitOnSocketData(sockets[0], 0, 0));

  test::closeTestFd(sockets[1]);
}
