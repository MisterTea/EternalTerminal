#ifndef __ET_JUMPHOST_PENDING__
#define __ET_JUMPHOST_PENDING__

#include "Connection.hpp"
#include "ETerminal.pb.h"
#include "Packet.hpp"
#include "WriteBuffer.hpp"

namespace et {

/**
 * @brief Userspace queue of jumphost packets waiting to go to the client.
 *
 * Only TERMINAL_BUFFER packets are dropped on interrupt; keepalives and
 * port-forward traffic must stay in order or the protocol desyncs.
 */
class JumphostPending {
 public:
  bool canAcceptMore() const { return bytes < WriteBuffer::MAX_BUFFER_SIZE; }

  size_t size() const { return bytes; }

  size_t terminalOutputBytes() const { return terminalBufferBytes; }

  bool empty() const { return packets.empty(); }

  void enqueue(const Packet& packet) {
    packets.push_back(packet);
    bytes += packet.length();
    if (packet.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
      terminalBufferBytes += packet.length();
    }
  }

  /**
   * @brief If terminal output exceeds FLUSH_THRESHOLD, drop those packets.
   * Control packets are left in place.
   * @return Bytes of TERMINAL_BUFFER discarded, or 0 if below threshold.
   */
  size_t flushTerminalBuffersIfLarge() {
    if (terminalBufferBytes < WriteBuffer::FLUSH_THRESHOLD) {
      return 0;
    }
    size_t dropped = 0;
    for (auto it = packets.begin(); it != packets.end();) {
      if (it->getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
        dropped += it->length();
        bytes -= it->length();
        terminalBufferBytes -= it->length();
        it = packets.erase(it);
      } else {
        ++it;
      }
    }
    return dropped;
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

 private:
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
};

}  // namespace et

#endif  // __ET_JUMPHOST_PENDING__
