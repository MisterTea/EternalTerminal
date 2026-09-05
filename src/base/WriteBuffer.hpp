#ifndef __ET_WRITE_BUFFER__
#define __ET_WRITE_BUFFER__

#include "Headers.hpp"
#include "TmuxCcFilter.hpp"

namespace et {

/**
 * @brief Bounded userspace queue for terminal output that has not yet been
 * written to the client socket.
 *
 * Unsent bytes live here so Ctrl+C (and similar interrupt bytes) can drop a
 * large backlog. Nested tmux -CC notifications that are not pane stdout are
 * kept (see {@link filterTmuxCc}). Sequence numbers are assigned only when a
 * chunk is later passed to writePacket(), so a flush never desynchronizes
 * reconnect recovery. The hard cap prevents a firehose from growing RAM
 * without bound while the client is connected; disconnect buffering still
 * uses BackedWriter.
 */
class WriteBuffer {
 public:
  /** @brief Drop the queue on interrupt only if it is at least this large. */
  static constexpr size_t FLUSH_THRESHOLD = 64 * 1024;
  /**
   * @brief Stop accepting PTY bytes (while connected) once the queue reaches
   * this size. 16MB is in the same class as a large autotuned TCP send
   * buffer, not the 64KB always-on backpressure that would stall a job
   * after a brief disconnect.
   */
  static constexpr size_t MAX_BUFFER_SIZE = 16 * 1024 * 1024;

  WriteBuffer()
      : totalBytes(0),
        writeOffset(0),
        skipUntilNewline(false),
        seenControlMode(false),
        controlPrefixBytes(0),
        controlPromoted(false) {}

  /**
   * @brief True if @p s contains a TTY interrupt byte (Ctrl+C/Z/\).
   */
  static bool containsInterruptByte(const string& s) {
    return s.find('\x03') != string::npos || s.find('\x1a') != string::npos ||
           s.find('\x1c') != string::npos;
  }

  /**
   * @brief Returns true if the buffer has room for more data.
   *
   * This is a soft bound: a whole chunk is admitted whenever size() is
   * below the limit, so the buffer can exceed MAX_BUFFER_SIZE by up to one
   * chunk.
   */
  bool canAcceptMore() const { return totalBytes < MAX_BUFFER_SIZE; }

  /** @brief True if interrupt should drop the pending queue. */
  bool shouldFlushOnInterrupt() const { return totalBytes >= FLUSH_THRESHOLD; }

  /** @brief Returns true if there is data waiting to be written. */
  bool hasPendingData() const { return !pending.empty(); }

  /** @brief Returns the current amount of buffered data in bytes. */
  size_t size() const { return totalBytes; }

  /**
   * @brief Adds data to the end of the buffer.
   *
   * After a flush drops an incomplete `%output`/TTY line, bytes up to the
   * next newline are discarded so the rest of that line cannot reappear.
   */
  void enqueue(const string& data) {
    string remaining = data;
    if (skipUntilNewline) {
      size_t newline = remaining.find('\n');
      if (newline == string::npos) {
        return;
      }
      skipUntilNewline = false;
      remaining = remaining.substr(newline + 1);
    }
    if (remaining.empty()) {
      return;
    }
    if (remaining[0] == '%' || remaining.find("\n%") != string::npos) {
      seenControlMode = true;
    }
    pending.push_back(remaining);
    totalBytes += remaining.size();
    controlPromoted = false;
    controlPrefixBytes = 0;
  }

  /**
   * @brief Returns a pointer to the next bytes to write and the count.
   * @param count Output: number of bytes available for writing.
   * @return Pointer to the data, or nullptr if buffer is empty.
   */
  const char* peekData(size_t* count) const {
    if (pending.empty()) {
      *count = 0;
      return nullptr;
    }
    const string& front = pending.front();
    *count = front.size() - writeOffset;
    return front.data() + writeOffset;
  }

  /**
   * @brief Removes bytesWritten from the front of the buffer.
   */
  void consume(size_t bytesWritten) {
    if (bytesWritten == 0) {
      return;
    }
    if (controlPrefixBytes > 0) {
      size_t take = std::min(bytesWritten, controlPrefixBytes);
      controlPrefixBytes -= take;
    }

    while (bytesWritten > 0 && !pending.empty()) {
      string& front = pending.front();
      size_t available = front.size() - writeOffset;

      if (bytesWritten >= available) {
        bytesWritten -= available;
        totalBytes -= available;
        writeOffset = 0;
        pending.pop_front();
      } else {
        writeOffset += bytesWritten;
        totalBytes -= bytesWritten;
        bytesWritten = 0;
      }
    }
  }

  /**
   * @brief Drops all pending data. Returns the number of bytes discarded.
   */
  size_t clear() {
    size_t dropped = totalBytes;
    pending.clear();
    totalBytes = 0;
    writeOffset = 0;
    skipUntilNewline = false;
    seenControlMode = false;
    controlPrefixBytes = 0;
    controlPromoted = false;
    return dropped;
  }

  /**
   * @brief Drop TTY / `%output` bytes, keeping tmux -CC control lines.
   * @return Bytes discarded.
   */
  size_t filterDroppable() {
    if (pending.empty()) {
      return 0;
    }
    string data;
    data.reserve(totalBytes);
    bool first = true;
    for (const string& chunk : pending) {
      if (first) {
        data.append(chunk, writeOffset, string::npos);
        first = false;
      } else {
        data.append(chunk);
      }
    }
    bool wasSkipping = skipUntilNewline;
    TmuxCcFilterResult result = filterTmuxCc(data, seenControlMode);
    // drainDiscardReadableBytes may re-filter the resync newline while the
    // rest of the dropped `%output` line is still arriving. enqueue() is
    // the only path that should clear skipUntilNewline (when it sees `\n`).
    skipUntilNewline = result.skipUntilNewline || wasSkipping;
    pending.clear();
    writeOffset = 0;
    totalBytes = 0;
    controlPrefixBytes = 0;
    controlPromoted = true;
    if (!result.kept.empty()) {
      pending.push_back(result.kept);
      totalBytes = result.kept.size();
      // Mid-line %output remainder is kept for protocol integrity but is not
      // a control-notification prefix.
      if (tmuxCcStartsAtLineBoundary(result.kept)) {
        controlPrefixBytes = result.kept.size();
      }
    }
    return result.dropped;
  }

  /**
   * @brief Move tmux -CC notifications ahead of pane stdout so a GUI
   * waiting on `%end` can issue `send-keys` (Ctrl+C) while the flood is
   * still sitting in this buffer.
   */
  void promoteControlLines() {
    if (!seenControlMode || controlPromoted || pending.empty()) {
      return;
    }
    string data;
    data.reserve(totalBytes);
    bool first = true;
    for (const string& chunk : pending) {
      if (first) {
        data.append(chunk, writeOffset, string::npos);
        first = false;
      } else {
        data.append(chunk);
      }
    }
    TmuxCcFilterResult result = filterTmuxCc(data, seenControlMode, false);
    pending.clear();
    writeOffset = 0;
    totalBytes = 0;
    controlPrefixBytes = 0;
    if (!result.kept.empty()) {
      pending.push_back(result.kept);
      totalBytes += result.kept.size();
      controlPrefixBytes = result.kept.size();
    }
    if (!result.droppable.empty()) {
      pending.push_back(result.droppable);
      totalBytes += result.droppable.size();
    }
    controlPromoted = true;
  }

  /** @brief Bytes of promoted control data still at the front of the queue. */
  size_t controlBytesAtFront() const { return controlPrefixBytes; }

  /** @brief True after `%output` / `%extended-output` has been seen. */
  bool inControlMode() const { return seenControlMode; }

  /**
   * @brief If the queue is at least FLUSH_THRESHOLD, drop droppable bytes.
   * @return Bytes discarded, or 0 if the queue was too small to flush.
   */
  size_t flushIfLarge() {
    if (!shouldFlushOnInterrupt()) {
      return 0;
    }
    return filterDroppable();
  }

  /** @brief True when the next enqueue should discard through a newline. */
  bool skippingUntilNewline() const { return skipUntilNewline; }

 private:
  std::deque<string> pending;
  size_t totalBytes;
  size_t writeOffset;
  bool skipUntilNewline;
  bool seenControlMode;
  size_t controlPrefixBytes;
  bool controlPromoted;
};
}  // namespace et

#endif  // __ET_WRITE_BUFFER__
