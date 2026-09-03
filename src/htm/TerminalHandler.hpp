#ifndef __HTM_TERMINAL_HANDLER__
#define __HTM_TERMINAL_HANDLER__

#include "Headers.hpp"

namespace et {
/**
 * @brief Spawns a pseudo-terminal and buffers data flowing through it.
 *
 * Used by `MultiplexerState` to collect pane output and replay buffered lines
 * when a client reconnects. Unix uses `forkpty`; Windows uses ConPTY.
 */
class TerminalHandler {
 public:
  /** @brief Sets up internal buffers/state before launching a PTY. */
  TerminalHandler();
  /** @brief Stops the child PTY if it is still running. */
  ~TerminalHandler();
  /** @brief Forks a child shell connected to a pty for interactive
   * input/output. */
  void start(const string& cwd = "", int cols = 80, int rows = 24);
  /** @brief Child process id, or 0 if unknown. */
  int64_t childProcessId() const;
  /** @brief Foreground process name on the PTY (tmux #{pane_current_command}). */
  string foregroundCommand() const;
  /**
   * @brief Drains available bytes from the pty, buffering them and returning
   * the raw bytes that were just read.
   */
  string pollUserTerminal();
  /** @brief Updates the terminal window size. */
  void updateTerminalSize(int col, int row);
  /** @brief Writes raw bytes into the running terminal (e.g., from the client).
   */
  void appendData(const string& data);
  /** @brief Indicates whether the PTY child is still alive. */
  inline bool isRunning() {
#ifdef WIN32
    if (processHandle == INVALID_HANDLE_VALUE) {
      return false;
    }
    return WaitForSingleObject(static_cast<HANDLE>(processHandle), 0) !=
           WAIT_OBJECT_0;
#else
    return run;
#endif
  }
  /** @brief Stops the handler's child process and closes PTY handles. */
  void stop();
  /** @brief Returns the buffered output that should be sent to the client. */
  const deque<string>& getBuffer() { return buffer; }

 protected:
  /** @brief Appends freshly read PTY bytes to the scrollback ring. */
  string bufferOutput(const string& newChars);
#ifndef WIN32
  /** @brief Writes as much of `pendingWrite` as the PTY will accept. */
  void flushPendingWrite();
#endif

#ifdef WIN32
  /** @brief ConPTY handle (`HPCON`). */
  void* hPC;
  /** @brief Write end of the pipe feeding ConPTY input. */
  void* inputWrite;
  /** @brief Read end of the pipe receiving ConPTY output. */
  void* outputRead;
  /** @brief Child process handle. */
  void* processHandle;
  /** @brief Continuously drains ConPTY's synchronous output channel. */
  thread outputThread;
  /** @brief Guards output waiting to be consumed by pollUserTerminal(). */
  mutex pendingOutputMutex;
  /** @brief Bytes drained by outputThread and awaiting a poll. */
  string pendingOutput;
#else
  /** @brief Master fd used to read/write the PTY. */
  int masterFd;
  /** @brief Child process ID for the spawned terminal. */
  int childPid;
  /** @brief Bytes waiting to be written because the PTY input buffer is full.
   */
  string pendingWrite;
#endif
  /** @brief Flag that indicates whether the handler is live. */
  atomic<bool> run;
  /** @brief Recent fragments that have been read from the PTY. */
  deque<string> buffer;
  /** @brief Running length of the buffered data for tracking split sizes. */
  int64_t bufferLength;
};
}  // namespace et

#endif  // __HTM_TERMINAL_HANDLER__
