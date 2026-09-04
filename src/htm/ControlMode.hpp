#ifndef __HTM_CONTROL_MODE_H__
#define __HTM_CONTROL_MODE_H__

#include "Headers.hpp"
#include "SocketHandler.hpp"

namespace et {

/** @brief tmux-compatible version string so GUI clients enable modern flags. */
static const char* const HTM_TMUX_VERSION = "3.5a";

/** @brief DCS sequence that starts tmux -CC (ESC P 1000 p). */
static const char kControlModeDcs[] = {'\x1b', 'P', '1', '0',
                                       '0',    '0', 'p', '\0'};
/** @brief ST that ends tmux -CC (ESC \\). */
static const char kControlModeSt[] = {'\x1b', '\\', '\0'};

/** @brief Octal-escape bytes < 32 and backslash, matching tmux %output. */
string controlOctalEscape(const string& data);

/** @brief Split a tmux-style command line into argv (quotes/backslashes). */
vector<string> controlSplitArgs(const string& line);

struct ControlFlagSet {
  map<char, vector<string>> values;
  set<char> present;
  vector<string> positional;

  bool has(char f) const { return present.count(f) != 0; }
  string get(char f, const string& fallback = "") const {
    auto it = values.find(f);
    if (it == values.end() || it->second.empty()) {
      return fallback;
    }
    return it->second.back();
  }
  const vector<string>& all(char f) const {
    static const vector<string> empty;
    auto it = values.find(f);
    return it == values.end() ? empty : it->second;
  }
};

/** @brief Parse argv into command name, flags, and positional args. */
struct ParsedControlCommand {
  string name;
  ControlFlagSet flags;
};

ParsedControlCommand parseControlCommand(const string& line);

/** @brief Split a tmux command list on unquoted `;`. */
vector<string> splitControlCommandList(const string& line);

/**
 * @brief Writes control-mode lines to an IPC client, queuing notifications
 * while a %begin/%end block is open.
 */
class ControlWriter {
 public:
  ControlWriter();
  void setSocket(shared_ptr<SocketHandler> handler, int fd);
  void clearSocket();
  bool hasClient() const { return fd >= 0 && socketHandler; }

  /** Client-originated reply (%begin/%end flags bit 0 set). */
  void begin();
  /**
   * Unsolicited attach handshake. iTerm2 waits for `%begin t 1 0` / `%end t 1
   * 0` before sending commands (see PTYSession startTmuxMode).
   */
  void beginServerOriginated();
  void writeOutput(const string& text);
  void end();
  void error(const string& message);
  void notify(const string& line);
  void flushNotifications();

  uint32_t commandNumber() const { return cmdNumber; }

 private:
  void beginWithFlags(int flags);
  void writeGuard(const char* kind);
  void writeLine(const string& line);

  shared_ptr<SocketHandler> socketHandler;
  int fd;
  uint32_t cmdNumber;
  long long replyTime;
  int replyFlags;
  bool inReply;
  vector<string> pendingNotify;
};

}  // namespace et

#endif
