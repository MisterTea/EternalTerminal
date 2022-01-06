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
#ifdef WIN32
  // - Close these after CreateProcess of child application with pseudoconsole
  // object.
  HANDLE inputReadSide, outputWriteSide;

  // - Hold onto these and use them for communication with the child through the
  // pseudoconsole.
  HANDLE outputReadSide, inputWriteSide;

  HPCON hPC;

  unique_ptr<std::thread> readThread;

  mutex readBufferMutex;
  string readBuffer;

  void beginRead();
#else
  int masterFd;
  int childPid;
#endif
  bool run;
  deque<string> buffer;
  int64_t bufferLength;
};
}  // namespace et

#endif  // __HTM_TERMINAL_HANDLER__
