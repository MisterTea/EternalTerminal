#ifndef __ET_CONTROL_PROTOCOL_HPP__
#define __ET_CONTROL_PROTOCOL_HPP__

#include "Headers.hpp"
#include "RawSocketUtils.hpp"
#include "SessionScrollback.hpp"
#include "SessionTranscript.hpp"

/*
 * The wire format spoken on the local control socket between `etctl` and a
 * backgrounded `et --ctl` session.  It deliberately mirrors ET's own router
 * framing (a 1-byte type header + payload), unencrypted because the socket is
 * local and 0600.  Each message is:
 *
 *   [1 byte opcode][4 byte big-endian payload length][payload bytes]
 *
 * Requests carry ET's native vocabulary verbatim where one exists: WRITE is a
 * raw TERMINAL_BUFFER payload, RESIZE is a TerminalInfo protobuf.  READ/INFO/KILL
 * are the only additions, for things ET has no native packet for (read-at-cursor,
 * liveness, end-session).
 */
namespace et {

enum ControlOpcode : uint8_t {
  // Requests (etctl -> daemon)
  CTL_WRITE = 1,   // payload: raw input bytes to inject
  CTL_RESIZE = 2,  // payload: TerminalInfo protobuf
  CTL_READ = 3,    // payload: 8-byte big-endian int64 cursor (<0 => from oldest)
  CTL_INFO = 4,    // payload: empty
  CTL_KILL = 5,    // payload: empty
  CTL_SNIFF = 6,    // payload: 8-byte int64 record cursor (<0 => from oldest)
  CTL_WRITE_SECRET =
      7,  // payload: raw input bytes; like CTL_WRITE but redacted from transcript

  // Responses (daemon -> etctl)
  CTL_OK = 64,         // payload: empty
  CTL_ERR = 65,        // payload: error message (utf-8)
  CTL_READ_RESP = 66,  // payload: [8B nextCursor][1B truncated][data...]
  CTL_INFO_RESP = 67,  // payload: "key=value\n" lines
  CTL_SNIFF_RESP = 68,  // payload: [8B next][1B trunc][rec: 1B dir,4B len,bytes]*
};

namespace control_proto {

inline void appendInt64BE(string* s, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 7; i >= 0; i--) {
    s->push_back((char)((u >> (i * 8)) & 0xFF));
  }
}

inline int64_t readInt64BE(const char* p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; i++) {
    u = (u << 8) | (uint8_t)p[i];
  }
  return (int64_t)u;
}

// Write one framed message.  Throws on IO error (RawSocketUtils semantics).
inline void writeFrame(int fd, uint8_t opcode, const string& payload) {
  char header[5];
  header[0] = (char)opcode;
  uint32_t len = (uint32_t)payload.size();
  header[1] = (char)((len >> 24) & 0xFF);
  header[2] = (char)((len >> 16) & 0xFF);
  header[3] = (char)((len >> 8) & 0xFF);
  header[4] = (char)(len & 0xFF);
  RawSocketUtils::writeAll(fd, header, sizeof(header));
  if (len) {
    RawSocketUtils::writeAll(fd, &payload[0], len);
  }
}

/*
 * Read one framed message.  Returns false on EOF/closed; throws on other IO
 * errors.  On success sets *opcode and *payload.
 */
inline bool readFrame(int fd, uint8_t* opcode, string* payload) {
  char header[5];
  try {
    RawSocketUtils::readAll(fd, header, sizeof(header));
  } catch (const std::runtime_error&) {
    return false;  // peer closed before/at the header
  }
  *opcode = (uint8_t)header[0];
  uint32_t len = ((uint32_t)(uint8_t)header[1] << 24) |
                 ((uint32_t)(uint8_t)header[2] << 16) |
                 ((uint32_t)(uint8_t)header[3] << 8) |
                 ((uint32_t)(uint8_t)header[4]);
  payload->assign(len, '\0');
  if (len) {
    RawSocketUtils::readAll(fd, &(*payload)[0], len);
  }
  return true;
}

inline string encodeCursor(int64_t cursor) {
  string s;
  appendInt64BE(&s, cursor);
  return s;
}

inline int64_t decodeCursor(const string& payload) {
  if (payload.size() < 8) {
    return -1;  // treat malformed/empty as "from oldest"
  }
  return readInt64BE(&payload[0]);
}

inline string encodeReadResp(const ScrollbackRead& r) {
  string s;
  appendInt64BE(&s, r.nextCursor);
  s.push_back(r.truncated ? (char)1 : (char)0);
  s += r.data;
  return s;
}

inline ScrollbackRead decodeReadResp(const string& payload) {
  ScrollbackRead r;
  if (payload.size() < 9) {
    return r;
  }
  r.nextCursor = readInt64BE(&payload[0]);
  r.truncated = payload[8] != 0;
  r.data = payload.substr(9);
  return r;
}

inline string encodeTranscriptResp(const TranscriptRead& tr) {
  string s;
  appendInt64BE(&s, tr.nextCursor);
  s.push_back(tr.truncated ? (char)1 : (char)0);
  for (const TranscriptRecord& rec : tr.records) {
    s.push_back(rec.dir);
    uint32_t len = (uint32_t)rec.bytes.size();
    s.push_back((char)((len >> 24) & 0xFF));
    s.push_back((char)((len >> 16) & 0xFF));
    s.push_back((char)((len >> 8) & 0xFF));
    s.push_back((char)(len & 0xFF));
    s += rec.bytes;
  }
  return s;
}

inline TranscriptRead decodeTranscriptResp(const string& payload) {
  TranscriptRead tr;
  if (payload.size() < 9) {
    return tr;
  }
  tr.nextCursor = readInt64BE(&payload[0]);
  tr.truncated = payload[8] != 0;
  size_t i = 9;
  while (i + 5 <= payload.size()) {
    char dir = payload[i];
    uint32_t len = ((uint32_t)(uint8_t)payload[i + 1] << 24) |
                   ((uint32_t)(uint8_t)payload[i + 2] << 16) |
                   ((uint32_t)(uint8_t)payload[i + 3] << 8) |
                   ((uint32_t)(uint8_t)payload[i + 4]);
    i += 5;
    if (i + len > payload.size()) {
      break;
    }
    tr.records.push_back(TranscriptRecord{dir, payload.substr(i, len)});
    i += len;
  }
  return tr;
}

}  // namespace control_proto
}  // namespace et

#endif  // __ET_CONTROL_PROTOCOL_HPP__
