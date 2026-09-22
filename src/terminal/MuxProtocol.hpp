#ifndef __ET_MUX_PROTOCOL__
#define __ET_MUX_PROTOCOL__

#include "Headers.hpp"

namespace et {

// OpenSSH PROTOCOL.mux message types (SSHMUX_VER = 4).
constexpr uint32_t SSHMUX_VER = 4;

constexpr uint32_t MUX_MSG_HELLO = 0x00000001;
constexpr uint32_t MUX_C_NEW_SESSION = 0x10000002;
constexpr uint32_t MUX_C_ALIVE_CHECK = 0x10000004;
constexpr uint32_t MUX_C_TERMINATE = 0x10000005;
constexpr uint32_t MUX_C_OPEN_FWD = 0x10000006;
constexpr uint32_t MUX_C_CLOSE_FWD = 0x10000007;
constexpr uint32_t MUX_C_NEW_STDIO_FWD = 0x10000008;
constexpr uint32_t MUX_C_STOP_LISTENING = 0x10000009;

constexpr uint32_t MUX_S_OK = 0x80000001;
constexpr uint32_t MUX_S_PERMISSION_DENIED = 0x80000002;
constexpr uint32_t MUX_S_FAILURE = 0x80000003;
constexpr uint32_t MUX_S_EXIT_MESSAGE = 0x80000004;
constexpr uint32_t MUX_S_ALIVE = 0x80000005;
constexpr uint32_t MUX_S_SESSION_OPENED = 0x80000006;
constexpr uint32_t MUX_S_REMOTE_PORT = 0x80000007;
constexpr uint32_t MUX_S_TTY_ALLOC_FAIL = 0x80000008;

constexpr uint32_t MUX_FWD_LOCAL = 1;
constexpr uint32_t MUX_FWD_REMOTE = 2;
constexpr uint32_t MUX_FWD_DYNAMIC = 3;

enum class ControlMasterMode { No, Yes, Auto };

struct ControlPersistConfig {
  bool enabled = false;
  // Seconds to keep the master after the last client exits. 0 means forever
  // (ControlPersist=yes). Ignored when enabled is false.
  int seconds = 0;
};

struct MuxOptions {
  ControlMasterMode controlMaster = ControlMasterMode::No;
  string controlPath;
  ControlPersistConfig controlPersist;
  // -O check|exit|stop|forward|cancel (empty when unused).
  string ctlCommand;
  // Non-Control* -o values retained for pass-through / future use.
  vector<string> passthroughOptions;
};

/** @brief Big-endian SSH uint32 / string helpers used by PROTOCOL.mux. */
class MuxBuffer {
 public:
  void clear();
  void putU32(uint32_t value);
  void putBool(bool value);
  void putString(const string& value);
  void putBytes(const void* data, size_t len);

  bool getU32(uint32_t* value);
  bool getBool(bool* value);
  bool getString(string* value);

  const string& data() const { return buf; }
  size_t size() const { return buf.size(); }
  size_t remaining() const { return buf.size() - offset; }
  void resetRead() { offset = 0; }
  void assign(string bytes) {
    buf = std::move(bytes);
    offset = 0;
  }

 private:
  string buf;
  size_t offset = 0;
};

/** @brief Framed mux packet I/O: uint32 length + body (type + payload). */
class MuxConnection {
 public:
  explicit MuxConnection(int fd, bool takeOwnership = true);
  ~MuxConnection();

  MuxConnection(const MuxConnection&) = delete;
  MuxConnection& operator=(const MuxConnection&) = delete;

  int fd() const { return sockFd; }
  bool valid() const { return sockFd >= 0; }
  /** @brief Release ownership without closing; returns the raw fd. */
  int release();

  bool writePacket(const MuxBuffer& body);
  bool readPacket(MuxBuffer* body, int timeoutMs = -1);

  // OpenSSH passenger mode: send/recv SCM_RIGHTS descriptors.
  bool sendFd(int fdToSend);
  bool recvFd(int* outFd);

  void close();

 private:
  int sockFd;
  bool ownsFd;
};

bool muxWriteAll(int fd, const char* data, size_t len);
bool muxReadExact(int fd, char* data, size_t len, int timeoutMs);
/** @brief Wait until fd is readable (events bit POLLIN) or writable (POLLOUT).
 */
int muxPollFd(int fd, short events, int timeoutMs);

struct MuxParseResult {
  MuxOptions options;
  // Remaining argv including argv[0], with mux flags removed.
  vector<string> remainingArgs;
};

/** @brief Parse ControlMaster / ControlPath / ControlPersist / -M / -S / -O. */
MuxParseResult parseMuxCliOptions(int argc, char** argv);

ControlMasterMode parseControlMasterValue(const string& value);
ControlPersistConfig parseControlPersistValue(const string& value);
bool controlPathSocketExists(const string& path);

string expandControlPath(const string& path);

}  // namespace et

#endif  // __ET_MUX_PROTOCOL__
