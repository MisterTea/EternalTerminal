#ifndef __HTM_TERMINAL_HANDLER__
#define __HTM_TERMINAL_HANDLER__

#include "Headers.hpp"

namespace et {
class TerminalHandler {
 public:
  TerminalHandler();
  void start();
  string pollUserTerminal();
  void updateTerminalSize(int col, int row);
  void appendData(const string &data);
  inline bool isRunning() { return run; }
  void stop();
  const deque<string> &getBuffer() { return buffer; }

 protected:
  int masterFd;
  int childPid;
  bool run;
  deque<string> buffer;
};
}  // namespace et

#endif  // __HTM_TERMINAL_HANDLER__
