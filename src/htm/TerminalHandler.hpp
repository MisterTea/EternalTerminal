#ifndef __HTM_TERMINAL_HANDLER__
#define __HTM_TERMINAL_HANDLER__

#include "Headers.hpp"

namespace et {
/**
 * @brief Spawns a pseudo-terminal and buffers data flowing through it.
 *
 * Used by `MultiplexerState` to collect pane output and replay buffered lines
 * when a client reconnects.
 */
class TerminalHandler {
 public:
  /** @brief Sets up internal buffers/state before launching a PTY. */
  TerminalHandler();
  /** @brief Forks a child shell connected to a pty for interactive
   * input/output. */
  void start();
  /**
   * @brief Drains available bytes from the pty, buffering them and returning
   * the raw bytes that were just read.
   */
  string pollUserTerminal();
  /** @brief Updates the terminal window size using TIOCSWINSZ. */
  void updateTerminalSize(int col, int row);
  /** @brief Writes raw bytes into the running terminal (e.g., from the client).
   */
  void appendData(const string &data);
  /** @brief Indicates whether the PTY child is still alive. */
  inline bool isRunning() { return run; }
  /** @brief Sends SIGTERM/Cleanup to stop the handler's child process. */
  void stop();
  /** @brief Returns the buffered output that should be sent to the client. */
  const deque<string> &getBuffer() { return buffer; }

 protected:
  /** @brief Master fd used to read/write the PTY. */
  int masterFd;
  /** @brief Child process ID for the spawned terminal. */
  int childPid;
  /** @brief Flag that indicates whether the handler is live. */
  bool run;
  /** @brief Recent fragments that have been read from the PTY. */
  deque<string> buffer;
  /** @brief Running length of the buffered data for tracking split sizes. */
  int64_t bufferLength;
};
}  // namespace et

#endif  // __HTM_TERMINAL_HANDLER__
