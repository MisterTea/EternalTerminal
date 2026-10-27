#ifndef WIN32
#include "ETerminal.pb.h"
#include "PipeSocketHandler.hpp"
#include "TerminalInterruptDrain.hpp"
#include "TestHeaders.hpp"
#include "TestSocketPair.hpp"
#include "WriteBuffer.hpp"

using namespace et;

namespace {
class TestPipeSocketHandler : public PipeSocketHandler {
 public:
  void trackSocket(int fd) {
    addToActiveSockets(fd);
    initSocket(fd);
  }
};
}  // namespace

TEST_CASE("Interrupt drain preserves TERMINAL_EXIT_STATUS",
          "[TerminalInterruptDrain][RemoteExitStatus]") {
  int fds[2];
  REQUIRE(test::createTestSocketPair(fds) == 0);

  TestPipeSocketHandler handler;
  handler.trackSocket(fds[0]);
  handler.trackSocket(fds[1]);

  et::TerminalBuffer tb;
  tb.set_buffer("pane-flood");
  handler.writePacket(
      fds[0], Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));

  et::TerminalExitStatus tes;
  tes.set_exitcode(42);
  handler.writePacket(fds[0], Packet(TerminalPacketType::TERMINAL_EXIT_STATUS,
                                     protoToString(tes)));

  WriteBuffer buf;
  std::deque<Packet> preserved;
  auto sharedHandler =
      shared_ptr<SocketHandler>(&handler, [](SocketHandler*) {});
  drainDiscardReadableBytes(sharedHandler, fds[1], &buf, &preserved);

  REQUIRE(preserved.size() == 1);
  REQUIRE(preserved.front().getHeader() ==
          TerminalPacketType::TERMINAL_EXIT_STATUS);
  et::TerminalExitStatus got =
      stringToProto<et::TerminalExitStatus>(preserved.front().getPayload());
  REQUIRE(got.exitcode() == 42);

  ::close(fds[0]);
  ::close(fds[1]);
}
#endif
