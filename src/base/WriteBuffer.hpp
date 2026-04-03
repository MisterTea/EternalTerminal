#ifndef __ET_WRITE_BUFFER__
#define __ET_WRITE_BUFFER__

#include "Headers.hpp"

namespace et {
/**
 * @brief Bounded buffer for pending write data, enabling flow control.
 *
 * This buffer is used to queue outgoing data when the socket is not ready
 * to accept writes. By limiting the buffer size, we create natural
 * backpressure that propagates upstream when the consumer is slow.
 */
class WriteBuffer {
 public:
  /** @brief Maximum bytes to buffer before applying backpressure. */
  static constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;  // 256KB

  WriteBuffer() : totalBytes(0), writeOffset(0) {}

  /**
   * @brief Returns true if the buffer has room for more data.
   * When false, the caller should stop reading from the source.
   */
  bool canAcceptMore() const { return totalBytes < MAX_BUFFER_SIZE; }

  /**
   * @brief Returns true if there is data waiting to be written.
   */
  bool hasPendingData() const { return !pending.empty(); }

  /**
   * @brief Returns the current amount of buffered data in bytes.
   */
  size_t size() const { return totalBytes; }

  /**
   * @brief Adds data to the end of the buffer.
   * @param data The data to enqueue.
   */
  void enqueue(const string &data) {
    if (data.empty()) return;
    pending.push_back(data);
    totalBytes += data.size();
  }

  /**
   * @brief Returns a pointer to the next bytes to write and the count.
   * @param count Output: number of bytes available for writing.
   * @return Pointer to the data, or nullptr if buffer is empty.
   */
  const char *peekData(size_t *count) const {
    if (pending.empty()) {
      *count = 0;
      return nullptr;
    }
    const string &front = pending.front();
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
      string &front = pending.front();
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
  std::deque<string> pending;
  size_t totalBytes;
  size_t writeOffset;  // Offset into the front chunk for partial writes
};
}  // namespace et

#endif  // __ET_WRITE_BUFFER__
