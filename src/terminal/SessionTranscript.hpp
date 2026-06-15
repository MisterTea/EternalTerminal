#ifndef __ET_SESSION_TRANSCRIPT_HPP__
#define __ET_SESSION_TRANSCRIPT_HPP__

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace et {

/*
 * One direction-tagged chunk of the session exchange: '>' for input the agent
 * injected, '<' for output the session produced.
 */
struct TranscriptRecord {
  char dir;
  std::string bytes;
};

/*
 * The result of reading the transcript at a record cursor.  `nextCursor` is the
 * record index to pass on the next read; `truncated` is true when the requested
 * cursor had already been evicted.
 */
struct TranscriptRead {
  std::vector<TranscriptRecord> records;
  int64_t nextCursor = 0;
  bool truncated = false;
};

/*
 * A bounded, ordered log of both injected input and produced output, used by
 * `etctl peep` to tap the byte exchange (» sent / « received).  Records are
 * appended in processing order and addressed by a monotonic record index; the
 * ring evicts oldest records once a byte cap is exceeded.  This is separate from
 * the SessionScrollback (which is output-only and serves `read`), so peep can
 * show the two directions interleaved without disturbing reads.
 */
class SessionTranscript {
 public:
  static const size_t kDefaultCapBytes = 512 * 1024;

  explicit SessionTranscript(size_t capBytes = kDefaultCapBytes)
      : capBytes(capBytes ? capBytes : kDefaultCapBytes) {}

  void append(char dir, const std::string& bytes) {
    if (bytes.empty()) {
      return;
    }
    std::lock_guard<std::mutex> guard(mutex);
    records.push_back(TranscriptRecord{dir, bytes});
    retained += bytes.size();
    headIndex++;
    while (retained > capBytes && records.size() > 1) {
      retained -= records.front().bytes.size();
      records.pop_front();
      baseIndex++;
    }
  }

  TranscriptRead read(int64_t cursor) const {
    std::lock_guard<std::mutex> guard(mutex);
    TranscriptRead result;
    if (cursor < 0) {
      cursor = baseIndex;
    }
    if (cursor < baseIndex) {
      result.truncated = true;
      cursor = baseIndex;
    }
    for (int64_t idx = cursor; idx < headIndex; idx++) {
      result.records.push_back(records[(size_t)(idx - baseIndex)]);
    }
    result.nextCursor = headIndex;
    return result;
  }

  int64_t headCursor() const {
    std::lock_guard<std::mutex> guard(mutex);
    return headIndex;
  }

 private:
  mutable std::mutex mutex;
  std::deque<TranscriptRecord> records;
  size_t capBytes;
  size_t retained = 0;
  int64_t baseIndex = 0;
  int64_t headIndex = 0;
};

}  // namespace et

#endif  // __ET_SESSION_TRANSCRIPT_HPP__
