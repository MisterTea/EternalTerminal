#include "ETerminal.pb.h"
#include "JumphostPending.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {
Packet terminalOutputPacket(const string& body) {
  et::TerminalBuffer tb;
  tb.set_buffer(body);
  return Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb));
}
}  // namespace

TEST_CASE("JumphostPending flush drops only large terminal output",
          "[JumphostPending]") {
  JumphostPending pending;
  pending.enqueue(Packet(TerminalPacketType::KEEP_ALIVE, ""));
  pending.enqueue(terminalOutputPacket(string(200 * 1024, 'A')));
  pending.enqueue(Packet(TerminalPacketType::KEEP_ALIVE, ""));
  REQUIRE(pending.terminalOutputBytes() >= WriteBuffer::FLUSH_THRESHOLD);
  REQUIRE(pending.flushTerminalBuffersIfLarge() > 0);
  REQUIRE(pending.terminalOutputBytes() == 0);

  REQUIRE(pending.packets.size() == 2);
  REQUIRE(pending.packets.front().getHeader() ==
          TerminalPacketType::KEEP_ALIVE);
  REQUIRE(pending.packets.back().getHeader() == TerminalPacketType::KEEP_ALIVE);
}

TEST_CASE("JumphostPending does not drop a small terminal queue",
          "[JumphostPending]") {
  JumphostPending pending;
  pending.enqueue(terminalOutputPacket("hello"));
  pending.enqueue(Packet(TerminalPacketType::KEEP_ALIVE, ""));
  REQUIRE(pending.flushTerminalBuffersIfLarge() == 0);
  REQUIRE(pending.packets.size() == 2);
  REQUIRE(pending.packets.front().getHeader() ==
          TerminalPacketType::TERMINAL_BUFFER);
}

TEST_CASE("JumphostPending Ctrl+C leaves the prompt packet next",
          "[JumphostPending]") {
  JumphostPending pending;
  pending.enqueue(terminalOutputPacket(string(200 * 1024, 'A')));
  REQUIRE(WriteBuffer::containsInterruptByte(string(1, '\x03')));
  REQUIRE(pending.flushTerminalBuffersIfLarge() > 0);
  pending.enqueue(terminalOutputPacket("PROMPT"));
  REQUIRE(pending.packets.size() == 1);
  et::TerminalBuffer tb =
      stringToProto<et::TerminalBuffer>(pending.packets.front().getPayload());
  REQUIRE(tb.buffer() == "PROMPT");
}

TEST_CASE("JumphostPending flush keeps tmux -CC control lines",
          "[JumphostPending][TmuxCc]") {
  JumphostPending pending;
  pending.enqueue(Packet(TerminalPacketType::KEEP_ALIVE, ""));
  pending.enqueue(terminalOutputPacket("%output %0 " + string(70 * 1024, 'y') +
                                       "\n"
                                       "%layout-change @1 layout vis flags\n"));
  pending.enqueue(Packet(TerminalPacketType::KEEP_ALIVE, ""));
  REQUIRE(pending.flushTerminalBuffersIfLarge() > 0);
  REQUIRE(pending.packets.size() == 3);
  REQUIRE(pending.packets[0].getHeader() == TerminalPacketType::KEEP_ALIVE);
  REQUIRE(pending.packets[1].getHeader() ==
          TerminalPacketType::TERMINAL_BUFFER);
  REQUIRE(pending.packets[2].getHeader() == TerminalPacketType::KEEP_ALIVE);
  et::TerminalBuffer tb =
      stringToProto<et::TerminalBuffer>(pending.packets[1].getPayload());
  REQUIRE(tb.buffer() == "%layout-change @1 layout vis flags\n");
}
