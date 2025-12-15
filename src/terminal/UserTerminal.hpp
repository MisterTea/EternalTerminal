#ifndef __USER_TERMINAL_HPP__
#define __USER_TERMINAL_HPP__

#include "ETerminal.pb.h"
#include "Headers.hpp"

namespace et {
/**
 * @brief Abstract terminal that can be started, resized, and observed through a
 * fd.
 */
class UserTerminal {
 public:
  virtual ~UserTerminal() {}

  /**
   * @brief Prepares the terminal and configures it using the router endpoint.
   * @param routerFd File descriptor that should be linked to the terminal
   * session.
   * @returns File descriptor used for reading incoming data (typically a master
   * pty).
   */
  virtual int setup(int routerFd) = 0;
  /** @brief Drives the interactive shell loop until the session exits. */
  virtual void runTerminal() = 0;
  /** @brief Blocks until the terminal child process ends and any cleanup
   * finishes. */
  virtual void handleSessionEnd() = 0;
  /** @brief Reclaims resources allocated by the terminal implementation. */
  virtual void cleanup() = 0;
  /** @brief Returns the descriptor that can be polled for terminal output. */
  virtual int getFd() = 0;
  /**
   * @brief Applies the current window geometry to the running terminal.
   * @param tmpwin Window size structure provided by the client.
   */
  virtual void setInfo(const winsize& tmpwin) = 0;
};
}  // namespace et

#endif
