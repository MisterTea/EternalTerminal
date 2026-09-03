#ifndef __ET_JUMPHOST_PENDING__
#define __ET_JUMPHOST_PENDING__

#include <algorithm>

#include "Connection.hpp"
#include "ETerminal.pb.h"
#include "Packet.hpp"
#include "TmuxCcFilter.hpp"
#include "WriteBuffer.hpp"

namespace et {

/**
 * @brief Userspace queue of jumphost packets waiting to go to the client.
 *
 * Only droppable terminal output is removed on interrupt; keepalives,
 * port-forward traffic, and tmux -CC control notifications stay in order.
 */
class JumphostPending {
 public:
  bool canAcceptMore() const { return bytes < WriteBuffer::MAX_BUFFER_SIZE; }

  size_t size() const { return bytes; }

  size_t terminalOutputBytes() const { return terminalBufferBytes; }

  bool empty() const { return packets.empty(); }

  void enqueue(const Packet& packet) {
    Packet toEnqueue = packet;
    if (skipUntilNewline &&
        toEnqueue.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
      string body = terminalBufferBody(toEnqueue);
      size_t newline = body.find('\n');
      if (newline == string::npos) {
        return;
      }
      skipUntilNewline = false;
      body = body.substr(newline + 1);
      if (body.empty()) {
        return;
      }
      toEnqueue = terminalBufferPacket(body);
    }
    packets.push_back(toEnqueue);
    bytes += toEnqueue.length();
    if (toEnqueue.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
      terminalBufferBytes += toEnqueue.length();
    }
  }

  /**
   * @brief Drop TTY / `%output` payloads; keep other packets and CC lines.
   * @return Bytes discarded from TERMINAL_BUFFER payloads.
   */
  size_t filterTerminalBuffers() {
    string concat;
    int firstTerminal = -1;
    for (size_t i = 0; i < packets.size(); ++i) {
      if (packets[i].getHeader() != TerminalPacketType::TERMINAL_BUFFER) {
        continue;
      }
      if (firstTerminal < 0) {
        firstTerminal = static_cast<int>(i);
      }
      concat += terminalBufferBody(packets[i]);
    }
    if (firstTerminal < 0) {
      return 0;
    }

    TmuxCcFilterResult result = filterTmuxCc(concat);
    skipUntilNewline = result.skipUntilNewline;

    for (auto it = packets.begin(); it != packets.end();) {
      if (it->getHeader() != TerminalPacketType::TERMINAL_BUFFER) {
        ++it;
        continue;
      }
      bytes -= it->length();
      terminalBufferBytes -= it->length();
      it = packets.erase(it);
    }

    size_t dropped = result.dropped;
    if (!result.kept.empty()) {
      Packet kept = terminalBufferPacket(result.kept);
      auto insertAt =
          packets.begin() +
          std::min(static_cast<size_t>(firstTerminal), packets.size());
      packets.insert(insertAt, kept);
      bytes += kept.length();
      terminalBufferBytes += kept.length();
    }

    return dropped;
  }

  /**
   * @brief If terminal output exceeds FLUSH_THRESHOLD, drop droppable bytes.
   * @return Bytes discarded, or 0 if below threshold.
   */
  size_t flushTerminalBuffersIfLarge() {
    if (terminalBufferBytes < WriteBuffer::FLUSH_THRESHOLD) {
      return 0;
    }
    return filterTerminalBuffers();
  }

  void drainToClient(Connection* conn, int serverClientFd) {
    if (packets.empty() || conn == nullptr) {
      return;
    }
    if (serverClientFd > 0) {
      if (!isSocketWritable(serverClientFd)) {
        return;
      }
      while (!packets.empty()) {
        popFrontToClient(conn);
        if (!isSocketWritable(serverClientFd)) {
          break;
        }
      }
      return;
    }
    while (!packets.empty()) {
      if (!conn->canBufferWrite(2 * 16 * 1024)) {
        break;
      }
      popFrontToClient(conn);
    }
  }

  std::deque<Packet> packets;

  bool skippingUntilNewline() const { return skipUntilNewline; }

 private:
  static Packet terminalBufferPacket(const string& body) {
    et::TerminalBuffer tb;
    tb.set_buffer(body);
    return Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb));
  }

  static string terminalBufferBody(const Packet& packet) {
    return stringToProto<et::TerminalBuffer>(packet.getPayload()).buffer();
  }

  void popFrontToClient(Connection* conn) {
    conn->writePacket(packets.front());
    bytes -= packets.front().length();
    if (packets.front().getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
      terminalBufferBytes -= packets.front().length();
    }
    packets.pop_front();
  }

  size_t bytes = 0;
  size_t terminalBufferBytes = 0;
  bool skipUntilNewline = false;
};

}  // namespace et

#endif  // __ET_JUMPHOST_PENDING__
