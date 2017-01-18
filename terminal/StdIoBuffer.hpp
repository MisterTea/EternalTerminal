#ifndef __STD_IO_BUFFER_H__
#define __STD_IO_BUFFER_H__

#include "Headers.hpp"

struct StdIoBuffer {
    StdIoBuffer() {
      oldStdout = cout.rdbuf(stdoutBuffer.rdbuf());
      oldStderr = cerr.rdbuf(stderrBuffer.rdbuf());
    }

    ~StdIoBuffer() {
      cout.rdbuf(oldStdout);
      cerr.rdbuf(oldStderr);
      cout << stdoutBuffer.str();
      cerr << stderrBuffer.str();
    }

private:
  streambuf* oldStdout;
  streambuf* oldStderr;
  stringstream stdoutBuffer,stderrBuffer;
};

#endif // __STD_IO_BUFFER_H__
