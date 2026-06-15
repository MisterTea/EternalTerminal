#ifndef __ET_CONTROL_CONSOLE_HPP__
#define __ET_CONTROL_CONSOLE_HPP__

#include "Console.hpp"
#include "Headers.hpp"
#include "SessionScrollback.hpp"
#include "SessionTranscript.hpp"

namespace et {

/*
 * A Console implementation that drives an ET session programmatically instead
 * of from a local TTY.  It is the seam that lets the *unmodified*
 * TerminalClient::run() loop be controlled by an external tool (etctl):
 *
 *   * getFd() returns the read end of an internal pipe.  run() selects on it and
 *     forwards whatever appears as TERMINAL_BUFFER input, so injectInput() bytes
 *     (keystrokes, 0x03, 0x04, escape sequences, anything) reach the remote PTY
 *     exactly as if typed.
 *   * write() is the output sink: run() hands every TERMINAL_BUFFER it receives
 *     here, and we append the raw bytes to a non-destructive scrollback that
 *     readOutput() serves by cursor.
 *   * getTerminalInfo() returns a settable size; setSize() changes it, and run()
 *     emits a TERMINAL_INFO (resize) on its next poll.
 *   * setup()/teardown() are no-ops: there is no TTY to put in raw mode.
 *
 * The pipe is kernel-synchronized; the scrollback is internally locked; the size
 * is guarded here.  run() touches this object on its main thread (getFd reads,
 * write, getTerminalInfo); the control listener touches it on another thread
 * (injectInput, readOutput, setSize) — a clean producer/consumer split.
 */
class ControlConsole : public Console {
 public:
  explicit ControlConsole(size_t scrollbackCapBytes =
                              SessionScrollback::kDefaultCapBytes);
  virtual ~ControlConsole();

  // --- Console interface (called by TerminalClient::run on its main thread) ---
  virtual TerminalInfo getTerminalInfo();
  virtual void setup() {}
  virtual void teardown() {}
  virtual int getFd();
  virtual void write(const string& s);

  // --- Control surface (called by the control listener thread) ---

  // Inject raw bytes as terminal input (returned to run() via getFd()).  When
  // `secret` is set the bytes still reach the shell, but the transcript records
  // a redacted placeholder so a `peep` never reveals a typed password.
  void injectInput(const string& bytes, bool secret = false);

  // Read session output at `cursor` without consuming it.
  ScrollbackRead readOutput(int64_t cursor) const;

  // Read the direction-tagged transcript (input + output) at a record cursor.
  TranscriptRead readTranscript(int64_t cursor) const;

  // Set the logical terminal size; run() will propagate it as a resize.
  void setSize(int row, int column, int width = 0, int height = 0);

  // The live output cursor (offset just past the most recent byte).
  int64_t headCursor() const { return scrollback.headCursor(); }

  // Seconds since the epoch of the last input or output activity.
  int64_t lastActivity() const;

  // Seconds since the epoch when this session was created.
  int64_t createdAt() const { return createdTime; }

 protected:
  SessionScrollback scrollback;
  SessionTranscript transcript;

  int inputPipe[2];  // [0] read end (getFd), [1] write end (injectInput)

  mutable std::mutex sizeMutex;
  TerminalInfo size;

  mutable std::mutex activityMutex;
  int64_t lastActivityTime;
  int64_t createdTime;

  void touchActivity();
};

}  // namespace et

#endif  // __ET_CONTROL_CONSOLE_HPP__
