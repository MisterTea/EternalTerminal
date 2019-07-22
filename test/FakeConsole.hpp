#ifndef __FAKE_CONSOLE_HPP__
#define __FAKE_CONSOLE_HPP__

#include "Console.hpp"
#include "UserTerminal.hpp"

#include "ETerminal.pb.h"
#include "PipeSocketHandler.hpp"

namespace et {
inline void consoleListenFn(shared_ptr<SocketHandler> socketHandler,
                            SocketEndpoint endpoint, int* serverClientFd) {
  // Only works when there is 1:1 mapping between endpoint and fds.  Will fix in
  // future api
  int serverFd = *(socketHandler->listen(endpoint).begin());
  int fd;
  while (true) {
    fd = socketHandler->accept(serverFd);
    if (fd == -1) {
      if (errno != EAGAIN) {
        FATAL_FAIL(fd);
      } else {
        ::usleep(100 * 1000);  // Sleep for client to connect
      }
    } else {
      break;
    }
  }
  *serverClientFd = fd;
}

class FakeConsole : public Console {
 public:
  FakeConsole(shared_ptr<PipeSocketHandler> _socketHandler)
      : socketHandler(_socketHandler), getTerminalInfoCount(0) {}

  virtual ~FakeConsole() {}

  virtual void setup() {
    fakeTerminalInfo.set_row(1);
    fakeTerminalInfo.set_column(1);
    fakeTerminalInfo.set_width(8);
    fakeTerminalInfo.set_height(10);

    string tmpPath = string("/tmp/et_test_console_XXXXXXXX");
    pipeDirectory = string(mkdtemp(&tmpPath[0]));
    pipePath = string(pipeDirectory) + "/pipe";
    SocketEndpoint endpoint(pipePath);
    serverClientFd = -1;
    std::thread serverListenThread(consoleListenFn, socketHandler, endpoint,
                                   &serverClientFd);
    // Wait for server to spin up
    ::usleep(1000 * 1000);
    clientServerFd = socketHandler->connect(endpoint);
    FATAL_FAIL(clientServerFd);
    serverListenThread.join();
    FATAL_FAIL(serverClientFd);
    LOG(ERROR) << "FDs: " << clientServerFd << " " << serverClientFd;
  }

  virtual void teardown() {
    socketHandler->close(clientServerFd);
    socketHandler->close(serverClientFd);
    FATAL_FAIL(::remove(pipePath.c_str()));
    FATAL_FAIL(::remove(pipeDirectory.c_str()));
  }

  virtual TerminalInfo getTerminalInfo() {
    getTerminalInfoCount++;
    if (getTerminalInfoCount % 100 == 0) {
      // Bump the terminal info
      fakeTerminalInfo.set_row(fakeTerminalInfo.row() + 1);
    }
    return fakeTerminalInfo;
  }

  virtual int getFd() { return clientServerFd; }

  string getTerminalData(int count) {
    string s(count, '\0');
    socketHandler->readAll(serverClientFd, &s[0], count, false);
    return s;
  }

  void simulateKeystrokes(const string& s) {
    LOG(ERROR) << "FDs: " << clientServerFd << " " << serverClientFd;
    socketHandler->writeAllOrThrow(serverClientFd, s.c_str(), s.length(),
                                   false);
  }

 protected:
  shared_ptr<PipeSocketHandler> socketHandler;
  TerminalInfo fakeTerminalInfo;
  int getTerminalInfoCount;
  int serverClientFd;
  int clientServerFd;
  string pipeDirectory;
  string pipePath;
};

class FakeUserTerminal : public UserTerminal {
 public:
  FakeUserTerminal(shared_ptr<PipeSocketHandler> _socketHandler)
      : socketHandler(_socketHandler),
        didCleanUp(false),
        didHandleSessionEnd(false) {
    memset(&lastWinInfo, 0, sizeof(winsize));
  }

  virtual ~FakeUserTerminal() {}

  virtual int setup(int routerFd) {
    string tmpPath = string("/tmp/et_test_userterminal_XXXXXXXX");
    pipeDirectory = string(mkdtemp(&tmpPath[0]));
    pipePath = string(pipeDirectory) + "/pipe";
    SocketEndpoint endpoint(pipePath);
    serverClientFd = -1;
    std::thread serverListenThread(consoleListenFn, socketHandler, endpoint,
                                   &serverClientFd);
    // Wait for server to spin up
    ::usleep(1000 * 1000);
    clientServerFd = socketHandler->connect(endpoint);
    FATAL_FAIL(clientServerFd);
    serverListenThread.join();
    FATAL_FAIL(serverClientFd);
    return getFd();
  };

  virtual void runTerminal(){

  };

  virtual int getFd() { return clientServerFd; }

  string getKeystrokes(int count) {
    string s(count, '\0');
    socketHandler->readAll(serverClientFd, &s[0], count, false);
    return s;
  }

  void simulateTerminalResponse(const string& s) {
    socketHandler->writeAllOrThrow(serverClientFd, s.c_str(), s.length(),
                                   false);
  }
  virtual void handleSessionEnd() { didHandleSessionEnd = true; }
  virtual void cleanup() { didCleanUp = true; }
  virtual void setInfo(const winsize& tmpwin) { lastWinInfo = tmpwin; }

 protected:
  shared_ptr<PipeSocketHandler> socketHandler;
  int serverClientFd;
  int clientServerFd;
  string pipeDirectory;
  string pipePath;
  bool didCleanUp;
  bool didHandleSessionEnd;
  winsize lastWinInfo;
};
}  // namespace et

#endif