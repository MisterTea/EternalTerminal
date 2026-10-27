#ifndef __CONSOLE_HPP__
#define __CONSOLE_HPP__

#include <optional>

#include "ETerminal.pb.h"
#include "Headers.hpp"

namespace et {
/** @brief Outcome of Console::readInput(). */
enum class ConsoleInputStatus {
  /** Input was appended to the output string. */
  DATA,
  /** Nothing to read right now. */
  NONE,
  /** No more input will arrive; keep the session running without it. */
  CLOSED,
  /** The interactive input went away; end the session. */
  FAILED,
};

/**
 * @brief Abstract console interface used by TerminalClient or terminal
 * emulators.
 *
 * The default I/O treats getFd() as a descriptor that reads keystrokes and
 * receives output (a raw fd on Unix, a socket on Windows; see ConsoleUnix.cpp
 * and ConsoleWindows.cpp).
 */
class Console {
 public:
  virtual ~Console() = default;

  /** @brief Returns console dimensions, or no value when they cannot be read.
   */
  virtual std::optional<TerminalInfo> getTerminalInfo() = 0;
  /** @brief Prepares the console/terminal before handing control to ET. */
  virtual void setup() = 0;
  /** @brief Restores the console state before exiting ET. */
  virtual void teardown() = 0;
  /** @brief Provides the descriptor that receives terminal output. */
  virtual int getFd() = 0;

  /** @brief Writes all of @p s (UTF-8) to the console. */
  virtual void write(const string& s);

  /**
   * @brief Write as many bytes as the console will accept without blocking.
   * @return Bytes written. 0 means try again when the fd is writable.
   */
  virtual size_t writeSome(const string& s);

  /**
   * @brief Descriptors whose readability means readInput() may have data.
   *
   * Empty means they cannot be polled, so readInput() is called every loop
   * iteration instead.
   */
  virtual vector<int> getInputPollFds() { return {getFd()}; }

  /**
   * @brief Descriptor to wait on for writability while output is pending, or
   * -1 when writeSome() never returns short.
   */
  virtual int getOutputPollFd() { return getFd(); }

  /**
   * @brief Reads available input without blocking.
   *
   * @param readyFds Readable descriptors from the last poll, which may
   *   include any of getInputPollFds().
   * @param out Receives the input when the result is DATA.
   */
  virtual ConsoleInputStatus readInput(const set<int>& readyFds, string* out);
};
}  // namespace et

#endif
