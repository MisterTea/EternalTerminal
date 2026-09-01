#ifndef __ET_WRITE_BUFFER__
#define __ET_WRITE_BUFFER__

#include "Headers.hpp"

namespace et {

/**
 * @brief Bounded userspace queue for terminal output that has not yet been
 * written to the client socket.
 *
 * Unsent bytes live here so Ctrl+C (and similar interrupt bytes) can drop a
 * large backlog. Sequence numbers are assigned only when a chunk is later
 * passed to writePacket(), so a flush never desynchronizes reconnect
 * recovery. The hard cap prevents a firehose from growing RAM without bound
 * while the client is connected; disconnect buffering still uses
 * BackedWriter.
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

  WriteBuffer() : totalBytes(0), writeOffset(0) {}

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
   */
  void enqueue(const string& data) {
    if (data.empty()) {
      return;
    }
    pending.push_back(data);
    totalBytes += data.size();
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
    return dropped;
  }

  /**
   * @brief If the queue is at least FLUSH_THRESHOLD, drop it.
   * @return Bytes discarded, or 0 if the queue was too small to flush.
   */
  size_t flushIfLarge() {
    if (!shouldFlushOnInterrupt()) {
      return 0;
    }
    return clear();
  }

 private:
  std::deque<string> pending;
  size_t totalBytes;
  size_t writeOffset;
};
}  // namespace et

#endif  // __ET_WRITE_BUFFER__
