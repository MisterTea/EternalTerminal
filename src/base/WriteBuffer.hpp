#ifndef __ET_WRITE_BUFFER__
#define __ET_WRITE_BUFFER__

#include "Headers.hpp"

namespace et {

enum class WriteBufferMode {
  BACKPRESSURE,  // Stop accepting data when buffer is full
  DISCARD        // Drop oldest data when buffer is full
};

/**
 * @brief Bounded buffer for pending write data, enabling flow control.
 *
 * Supports two modes:
 * - BACKPRESSURE: canAcceptMore() returns false when full, causing callers
 *   to stop reading from the source. Processes stall when consumer is slow.
 * - DISCARD: canAcceptMore() always returns true. When the buffer exceeds
 *   MAX_BUFFER_SIZE, oldest data is dropped. Processes never stall.
 */
class WriteBuffer {
 public:
  /** @brief Maximum bytes to buffer before applying backpressure or
   * discarding. Every byte held here (or in any buffer downstream) is stale
   * data the user must wait through on a slow link, so keep this small: at
   * 100KB/s, 64KB is ~0.6s of output. It only needs to absorb short bursts
   * between drain opportunities (reads are 16KB), not smooth over the
   * network. This is a soft bound: canAcceptMore() admits a whole chunk
   * whenever size() is below the limit, so the buffer can exceed it by up
   * to one chunk. */
  static constexpr size_t MAX_BUFFER_SIZE = 64 * 1024;  // 64KB

  explicit WriteBuffer(WriteBufferMode mode = WriteBufferMode::BACKPRESSURE)
      : mode(mode), totalBytes(0), writeOffset(0) {}

  /**
   * @brief Returns true if the buffer has room for more data.
   * In BACKPRESSURE mode, returns false when buffer >= MAX_BUFFER_SIZE.
   * In DISCARD mode, always returns true (old data will be dropped).
   */
  bool canAcceptMore() const {
    if (mode == WriteBufferMode::DISCARD) return true;
    return totalBytes < MAX_BUFFER_SIZE;
  }

  /**
   * @brief Returns true if there is data waiting to be written.
   */
  bool hasPendingData() const { return !pending.empty(); }

  /**
   * @brief Returns the current amount of buffered data in bytes.
   */
  size_t size() const { return totalBytes; }

  /**
   * @brief Returns the buffer mode.
   */
  WriteBufferMode getMode() const { return mode; }

  /**
   * @brief Adds data to the end of the buffer.
   * In DISCARD mode, drops oldest chunks if the buffer would exceed
   * MAX_BUFFER_SIZE.
   * @param data The data to enqueue.
   */
  void enqueue(const string& data) {
    if (data.empty()) return;
    pending.push_back(data);
    totalBytes += data.size();

    if (mode == WriteBufferMode::DISCARD) {
      discardOldest();
    }
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
   * @param bytesWritten Number of bytes successfully written to socket.
   */
  void consume(size_t bytesWritten) {
    if (bytesWritten == 0) return;

    while (bytesWritten > 0 && !pending.empty()) {
      string& front = pending.front();
      size_t available = front.size() - writeOffset;

      if (bytesWritten >= available) {
        // Consumed the entire front chunk
        bytesWritten -= available;
        totalBytes -= available;
        writeOffset = 0;
        pending.pop_front();
      } else {
        // Partial consumption
        writeOffset += bytesWritten;
        totalBytes -= bytesWritten;
        bytesWritten = 0;
      }
    }
  }

  /**
   * @brief Clears all pending data.
   */
  void clear() {
    pending.clear();
    totalBytes = 0;
    writeOffset = 0;
  }

 private:
  /**
   * @brief Drops oldest chunks until buffer is within MAX_BUFFER_SIZE.
   * Only called in DISCARD mode after enqueue.
   */
  void discardOldest() {
    while (totalBytes > MAX_BUFFER_SIZE && !pending.empty()) {
      string& front = pending.front();
      size_t frontSize = front.size() - writeOffset;
      totalBytes -= frontSize;
      writeOffset = 0;
      pending.pop_front();
    }
  }

  WriteBufferMode mode;
  std::deque<string> pending;
  size_t totalBytes;
  size_t writeOffset;  // Offset into the front chunk for partial writes
};
}  // namespace et

#endif  // __ET_WRITE_BUFFER__
