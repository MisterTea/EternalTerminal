#ifndef __STDIO_CONSOLE_HPP__
#define __STDIO_CONSOLE_HPP__

#include "Console.hpp"

namespace et {
/**
 * @brief A console that reads the process's stdin and writes its stdout.
 *
 * On Unix both descriptors are polled. Windows CRT descriptors are not
 * sockets and cannot go through WSAPoll, so readInput() checks the standard
 * input handle directly every loop iteration (see ConsoleWindows.cpp).
 */
class StdioConsole : public Console {
 public:
  /** @brief Makes stdin and stdout non-blocking where the platform allows. */
  void setup() override;
  int getFd() override { return STDOUT_FILENO; }
  vector<int> getInputPollFds() override;
  int getOutputPollFd() override;
  ConsoleInputStatus readInput(const set<int>& readyFds, string* out) override;
};
}  // namespace et

#endif  // __STDIO_CONSOLE_HPP__
