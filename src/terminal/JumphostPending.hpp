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
    if (toEnqueue.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
      string body = terminalBufferBody(toEnqueue);
      if (!body.empty() &&
          (body[0] == '%' || body.find("\n%") != string::npos)) {
        seenControlMode = true;
      }
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

    bool wasSkipping = skipUntilNewline;
    TmuxCcFilterResult result = filterTmuxCc(concat, seenControlMode);
    skipUntilNewline = result.skipUntilNewline || wasSkipping;

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

  // Returns true when control notifications were just sent and a large
  // droppable backlog remains.
  bool drainToClient(Connection* conn, int serverClientFd) {
    if (packets.empty() || conn == nullptr) {
      return false;
    }
    promoteControlPackets();
    bool wroteControl = false;
    if (serverClientFd > 0) {
      while (!packets.empty()) {
        if (conn->hasData()) {
          return false;
        }
        if (!isSocketWritable(serverClientFd)) {
          break;
        }
        const bool droppable = frontIsDroppableOutput();
        if (droppable && wroteControl &&
            terminalBufferBytes >= WriteBuffer::FLUSH_THRESHOLD) {
          return true;
        }
        popFrontToClient(conn);
        if (!droppable) {
          wroteControl = true;
        }
      }
      return false;
    }
    while (!packets.empty()) {
      if (!conn->canBufferWrite(2 * 16 * 1024)) {
        break;
      }
      popFrontToClient(conn);
    }
    return false;
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

  bool frontIsDroppableOutput() const {
    if (packets.empty()) {
      return false;
    }
    if (packets.front().getHeader() != TerminalPacketType::TERMINAL_BUFFER) {
      return false;
    }
    string body = terminalBufferBody(packets.front());
    string token = tmuxCcFirstToken(body);
    return token.empty() || tmuxCcIsDroppableOutputToken(token) ||
           (!token.empty() && token[0] != '%');
  }

  void promoteControlPackets() {
    if (!seenControlMode || packets.empty()) {
      return;
    }
    string concat;
    vector<Packet> nonTerminal;
    for (const Packet& packet : packets) {
      if (packet.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
        concat += terminalBufferBody(packet);
      } else {
        nonTerminal.push_back(packet);
      }
    }
    TmuxCcFilterResult result = filterTmuxCc(concat, seenControlMode, false);
    packets.clear();
    bytes = 0;
    terminalBufferBytes = 0;
    for (const Packet& packet : nonTerminal) {
      packets.push_back(packet);
      bytes += packet.length();
    }
    if (!result.kept.empty()) {
      Packet kept = terminalBufferPacket(result.kept);
      packets.push_back(kept);
      bytes += kept.length();
      terminalBufferBytes += kept.length();
    }
    if (!result.droppable.empty()) {
      const string& droppable = result.droppable;
      for (size_t offset = 0; offset < droppable.size();
           offset += size_t(16 * 1024)) {
        Packet chunk =
            terminalBufferPacket(droppable.substr(offset, size_t(16 * 1024)));
        packets.push_back(chunk);
        bytes += chunk.length();
        terminalBufferBytes += chunk.length();
      }
    }
  }

  size_t bytes = 0;
  size_t terminalBufferBytes = 0;
  bool skipUntilNewline = false;
  bool seenControlMode = false;
};

}  // namespace et

#endif  // __ET_JUMPHOST_PENDING__
