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

/** @brief `-t` / `-T` after the pre-pass. Later flag wins. */
enum class PtyOverride { Default, Force, Disable };

/**
 * @brief OpenSSH short flags stripped before cxxopts. Long options such as
 * `--port` and `--tunnel` stay on the remaining argv.
 */
struct OpenSshClientFlags {
  bool sshPortSet = false;
  int sshPort = 22;
  bool loginNameSet = false;
  string loginName;
  bool cipherSet = false;
  string cipher;
  PtyOverride pty = PtyOverride::Default;
  bool background = false;
  bool noRemoteCommand = false;
  bool disableX11 = false;
  int verboseCount = 0;
  bool escapeSet = false;
  string escapeChar;
  vector<string> localForwards;
  vector<string> remoteForwards;
  vector<string> identityFiles;
  bool jumpHostSet = false;
  string jumpHost;
  // Index in the post-argv0 argument list. Compared with explicitJumpHostIndex
  // so the later of `-J` and `--jumphost` wins.
  int jumpHostArgIndex = -1;
  int explicitJumpHostIndex = -1;
};

struct MuxParseResult {
  MuxOptions options;
  OpenSshClientFlags ssh;
  // Remaining argv including argv[0], with mux flags and OpenSSH short flags
  // removed. `-T`, `-D`, `-W`, `-F`, `-o`, and other cxxopts shorts are
  // re-emitted as separate tokens.
  vector<string> remainingArgs;
};

/** @brief Join `--tunnel` / `-L` (or reverse) specs the parser already accepts.
 */
inline string mergeForwardSpecs(const string& base,
                                const vector<string>& extra) {
  string out = base;
  for (const auto& spec : extra) {
    if (spec.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += spec;
  }
  return out;
}

/** @brief `-N` is forwards-only and cannot carry a remote command. */
inline bool remoteCommandConflictsWithNoCommand(bool noRemoteCommand,
                                                const string& command) {
  return noRemoteCommand && !command.empty();
}

/**
 * @brief Whether the remote command runs on pipes instead of a pty. `-N` runs
 * no remote command, so `-T` has nothing to apply to.
 */
inline bool remotePtyDisabled(const OpenSshClientFlags& flags) {
  return flags.pty == PtyOverride::Disable && !flags.noRemoteCommand;
}

/** @brief Error for an unusable -N / -T / -W combination, or empty. */
inline string remoteCommandOptionsError(const OpenSshClientFlags& flags,
                                        const string& command,
                                        bool stdioForward) {
  if (remoteCommandConflictsWithNoCommand(flags.noRemoteCommand, command)) {
    return "-N cannot be combined with a remote command";
  }
  if (remotePtyDisabled(flags) && command.empty()) {
    return "-T/--no-pty requires --command or a positional command";
  }
  if (remotePtyDisabled(flags) && stdioForward) {
    return "-W/--stdio-forward cannot be combined with -T/--no-pty";
  }
  return "";
}

struct BootstrapSshPort {
  bool set = false;
  int port = 22;
};

/**
 * @brief The sshd port passed to the bootstrap ssh as `-p`, if any. Only an
 * explicit `-p` qualifies: ssh reads its own config Port, and a config-derived
 * `-p` would beat `--ssh-option Port=` because ssh keeps the first value.
 */
inline BootstrapSshPort bootstrapSshPort(const OpenSshClientFlags& flags) {
  return {flags.sshPortSet, flags.sshPort};
}

/**
 * @brief Later of `-J` and `--jumphost` wins. An absent flag leaves the other.
 */
inline string resolveJumpHost(const OpenSshClientFlags& flags, bool hasLongJump,
                              const string& longJump) {
  if (!flags.jumpHostSet) {
    return longJump;
  }
  if (!hasLongJump || flags.jumpHostArgIndex > flags.explicitJumpHostIndex) {
    return flags.jumpHost;
  }
  return longJump;
}

/** @brief Parse ControlMaster / ControlPath / ControlPersist / -M / -S / -O. */
MuxParseResult parseMuxCliOptions(int argc, char** argv);

ControlMasterMode parseControlMasterValue(const string& value);
ControlPersistConfig parseControlPersistValue(const string& value);
bool controlPathSocketExists(const string& path);

string expandControlPath(const string& path);

}  // namespace et

#endif  // __ET_MUX_PROTOCOL__
