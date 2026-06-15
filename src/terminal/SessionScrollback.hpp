#ifndef __ET_SESSION_SCROLLBACK_HPP__
#define __ET_SESSION_SCROLLBACK_HPP__

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace et {

/*
 * The result of reading the scrollback at a given cursor.
 *
 * `data` is the raw output bytes in the half-open range [requested_cursor,
 * nextCursor).  `nextCursor` is the cursor to pass on the next read to get only
 * newer bytes.  `truncated` is true when the requested cursor had already been
 * evicted (the reader fell behind the retained window), in which case `data`
 * begins at the oldest retained byte and the caller knows a gap was skipped.
 */
struct ScrollbackRead {
  std::string data;
  int64_t nextCursor = 0;
  bool truncated = false;
};

/*
 * A bounded, non-destructive, multi-reader ring of raw terminal output bytes.
 *
 * This is the read side of an ET control session: server output is `append`ed
 * as it arrives, and any number of independent readers `read` from their own
 * `int64_t` byte cursor without consuming anything.  It mirrors the pattern ET
 * already uses in htm's TerminalHandler (a deque<string> buffer plus a byte
 * count); the only additions are a monotonic absolute cursor and FIFO eviction.
 *
 * Cursors are absolute byte offsets from the start of the session.  `headOffset`
 * is the offset just past the most recent byte (the live cursor); `baseOffset`
 * is the offset of the oldest byte still retained.  Both only ever increase.
 */
class SessionScrollback {
 public:
  static const size_t kDefaultCapBytes = 2 * 1024 * 1024;

  explicit SessionScrollback(size_t capBytes = kDefaultCapBytes)
      : capBytes(capBytes ? capBytes : kDefaultCapBytes) {}

  /*
   * Append output bytes and advance the head cursor, evicting oldest chunks
   * once the retained size would exceed the cap.
   */
  void append(const std::string& bytes) {
    if (bytes.empty()) {
      return;
    }
    std::lock_guard<std::mutex> guard(mutex);
    headOffset += static_cast<int64_t>(bytes.size());
    chunks.push_back(bytes);
    retained += bytes.size();
    while (retained > capBytes && chunks.size() > 1) {
      retained -= chunks.front().size();
      baseOffset += static_cast<int64_t>(chunks.front().size());
      chunks.pop_front();
    }
  }

  /*
   * Return all retained bytes at or after `cursor` (non-destructive).  A cursor
   * below the retained window is clamped to `baseOffset` and flagged truncated;
   * a cursor at or beyond the head returns empty.  A negative cursor is treated
   * as "from the oldest retained byte".
   */
  ScrollbackRead read(int64_t cursor) const {
    std::lock_guard<std::mutex> guard(mutex);
    ScrollbackRead result;
    if (cursor < 0) {
      cursor = baseOffset;
    }
    if (cursor < baseOffset) {
      result.truncated = true;
      cursor = baseOffset;
    }
    if (cursor >= headOffset) {
      result.nextCursor = headOffset;
      return result;
    }
    result.data.reserve(static_cast<size_t>(headOffset - cursor));
    int64_t chunkStart = baseOffset;
    for (const auto& chunk : chunks) {
      const int64_t chunkEnd = chunkStart + static_cast<int64_t>(chunk.size());
      if (chunkEnd > cursor) {
        const size_t from = cursor > chunkStart
                                ? static_cast<size_t>(cursor - chunkStart)
                                : 0;
        result.data.append(chunk, from, std::string::npos);
      }
      chunkStart = chunkEnd;
    }
    result.nextCursor = headOffset;
    return result;
  }

  // The live cursor: the offset just past the most recently appended byte.
  int64_t headCursor() const {
    std::lock_guard<std::mutex> guard(mutex);
    return headOffset;
  }

  // The oldest retained offset (bytes before this have been evicted).
  int64_t baseCursor() const {
    std::lock_guard<std::mutex> guard(mutex);
    return baseOffset;
  }

  // Number of bytes currently retained in the ring.
  size_t size() const {
    std::lock_guard<std::mutex> guard(mutex);
    return retained;
  }

 private:
  mutable std::mutex mutex;
  std::deque<std::string> chunks;
  size_t capBytes;
  size_t retained = 0;
  int64_t baseOffset = 0;
  int64_t headOffset = 0;
};

}  // namespace et

#endif  // __ET_SESSION_SCROLLBACK_HPP__
