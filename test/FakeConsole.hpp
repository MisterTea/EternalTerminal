#ifndef __FAKE_CONSOLE_HPP__
#define __FAKE_CONSOLE_HPP__

#include <fcntl.h>

#include "Console.hpp"
#include "ETerminal.pb.h"
#include "PipeSocketHandler.hpp"
#include "UserTerminal.hpp"

namespace et {
class FakeConsole : public Console {
 public:
  FakeConsole(shared_ptr<PipeSocketHandler> _socketHandler)
      : socketHandler(_socketHandler),
        getTerminalInfoCount(0),
        terminalInfoAvailable(true),
        automaticallyChangeTerminalInfo(true),
        clientServerFd(-1) {}

  virtual ~FakeConsole() {}

  void consoleListenFn(shared_ptr<SocketHandler> socketHandler,
                       SocketEndpoint endpoint, int* serverClientFd) {
    // Only works when there is 1:1 mapping between endpoint and fds.  Will fix
    // in future api
    int serverFd = *(socketHandler->listen(endpoint).begin());
    int fd;
    while (true) {
      fd = socketHandler->accept(serverFd);
      if (fd == -1) {
        if (GetErrno() != EAGAIN && GetErrno() != EWOULDBLOCK) {
          FATAL_FAIL(fd);
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      } else {
        break;
      }
    }

    lock_guard<recursive_mutex> lock(_mutex);
    *serverClientFd = fd;
  }

  virtual void setup() {
    fakeTerminalInfo.set_row(1);
    fakeTerminalInfo.set_column(1);
    fakeTerminalInfo.set_width(8);
    fakeTerminalInfo.set_height(10);

#ifdef WIN32
    pipePath = "et_test_console_" + genRandomAlphaNum(12) + ".ipc";
#else
    string tmpPath = GetTempDirectory() + string("et_test_console_XXXXXXXX");
    pipeDirectory = string(mkdtemp(&tmpPath[0]));
    pipePath = string(pipeDirectory) + "/pipe";
#endif
    SocketEndpoint endpoint;
    endpoint.set_name(pipePath);
    {
      lock_guard<recursive_mutex> lock(_mutex);
      serverClientFd = -1;
    }
    std::thread serverListenThread(&FakeConsole::consoleListenFn, this,
                                   socketHandler, endpoint, &serverClientFd);
    // Wait for server to spin up
    std::this_thread::sleep_for(std::chrono::seconds(1));
    int fd = socketHandler->connect(endpoint);
    FATAL_FAIL(fd);
    {
      lock_guard<recursive_mutex> lock(_mutex);
      clientServerFd = fd;
    }
    serverListenThread.join();
    FATAL_FAIL(serverClientFd);
    LOG(INFO) << "FDs: " << clientServerFd << " " << serverClientFd;
  }

  virtual void teardown() {
    int localClientServerFd;
    int localServerClientFd;
    {
      lock_guard<recursive_mutex> lock(_mutex);
      localClientServerFd = clientServerFd;
      localServerClientFd = serverClientFd;
      clientServerFd = -1;
      serverClientFd = -1;
    }
    socketHandler->close(localClientServerFd);
    socketHandler->close(localServerClientFd);
    SocketEndpoint endpoint;
    endpoint.set_name(pipePath);
    socketHandler->stopListening(endpoint);
#ifndef WIN32
    FATAL_FAIL(::remove(pipeDirectory.c_str()));
#endif
  }

  virtual std::optional<TerminalInfo> getTerminalInfo() {
    lock_guard<recursive_mutex> lock(_mutex);
    getTerminalInfoCount++;
    if (!terminalInfoAvailable) {
      return std::nullopt;
    }
    if (automaticallyChangeTerminalInfo && getTerminalInfoCount % 100 == 0) {
      // Bump the terminal info
      fakeTerminalInfo.set_row(fakeTerminalInfo.row() + 1);
    }
    return fakeTerminalInfo;
  }

  void setTerminalInfoResult(const std::optional<TerminalInfo>& terminalInfo) {
    lock_guard<recursive_mutex> lock(_mutex);
    automaticallyChangeTerminalInfo = false;
    terminalInfoAvailable = terminalInfo.has_value();
    if (terminalInfo) {
      fakeTerminalInfo = *terminalInfo;
    }
  }

  virtual int getFd() {
    lock_guard<recursive_mutex> lock(_mutex);
    return clientServerFd;
  }

  void write(const string& data) override {
    int fd;
    {
      lock_guard<recursive_mutex> lock(_mutex);
      fd = clientServerFd;
    }
    socketHandler->writeAllOrThrow(fd, data.data(), data.size(), false);
  }

  bool isSetup() {
    lock_guard<recursive_mutex> lock(_mutex);
    return clientServerFd >= 0 && serverClientFd >= 0;
  }

  string getTerminalData(int count) {
    string s(count, '\0');
    int fd;
    {
      lock_guard<recursive_mutex> lock(_mutex);
      fd = serverClientFd;
    }
    socketHandler->readAll(fd, &s[0], count, false);
    return s;
  }

  void simulateKeystrokes(const string& s) {
    int localClientServerFd;
    int localServerClientFd;
    {
      lock_guard<recursive_mutex> lock(_mutex);
      localClientServerFd = clientServerFd;
      localServerClientFd = serverClientFd;
    }
    LOG(INFO) << "FDs: " << localClientServerFd << " " << localServerClientFd;
    socketHandler->writeAllOrThrow(localServerClientFd, s.c_str(), s.length(),
                                   false);
  }

 protected:
  recursive_mutex _mutex;
  shared_ptr<PipeSocketHandler> socketHandler;
  TerminalInfo fakeTerminalInfo;
  int getTerminalInfoCount;
  bool terminalInfoAvailable;
  bool automaticallyChangeTerminalInfo;
  int serverClientFd;
  int clientServerFd;
  string pipeDirectory;
  string pipePath;
};

class FakeUserTerminal : public UserTerminal {
 public:
  FakeUserTerminal(shared_ptr<PipeSocketHandler> _socketHandler)
      : socketHandler(_socketHandler),
        serverClientFd(-1),
        clientServerFd(-1),
        setInfoCount(0),
        didCleanUp(false),
        didHandleSessionEnd(false) {
    memset(&lastWinInfo, 0, sizeof(winsize));
  }

  virtual ~FakeUserTerminal() {}

  void listenFn(shared_ptr<SocketHandler> socketHandler,
                SocketEndpoint endpoint, int* serverClientFd) {
    // Only works when there is 1:1 mapping between endpoint and fds.  Will fix
    // in future api
    int serverFd = *(socketHandler->listen(endpoint).begin());
    int fd;
    while (true) {
      fd = socketHandler->accept(serverFd);
      if (fd == -1) {
        if (GetErrno() != EAGAIN && GetErrno() != EWOULDBLOCK) {
          FATAL_FAIL(fd);
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      } else {
        break;
      }
    }
    lock_guard<recursive_mutex> lock(_mutex);
    *serverClientFd = fd;
  }

  virtual int setup(int routerFd) {
#ifdef WIN32
    pipePath = "et_test_userterminal_" + genRandomAlphaNum(12) + ".ipc";
#else
    string tmpPath =
        GetTempDirectory() + string("et_test_userterminal_XXXXXXXX");
    pipeDirectory = string(mkdtemp(&tmpPath[0]));
    pipePath = string(pipeDirectory) + "/pipe";
#endif
    SocketEndpoint endpoint;
    endpoint.set_name(pipePath);
    serverClientFd = -1;
    std::thread serverListenThread(&FakeUserTerminal::listenFn, this,
                                   socketHandler, endpoint, &serverClientFd);
    // Wait for server to spin up
    std::this_thread::sleep_for(std::chrono::seconds(1));
    clientServerFd = socketHandler->connect(endpoint);
    FATAL_FAIL(clientServerFd);
    serverListenThread.join();
    FATAL_FAIL(serverClientFd);
    // Honor the UserTerminal contract: the handler polls this fd non-blocking.
#ifdef WIN32
    u_long nonBlocking = 1;
    FATAL_FAIL(ioctlsocket(clientServerFd, FIONBIO, &nonBlocking));
#else
    int flags = fcntl(clientServerFd, F_GETFL, 0);
    if (flags != -1) {
      fcntl(clientServerFd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    return getFd();
  };

  virtual void runTerminal() {

  };

  virtual int getFd() { return clientServerFd; }

  string getKeystrokes(int count) {
    lock_guard<recursive_mutex> lock(_mutex);
    string s(count, '\0');
    socketHandler->readAll(serverClientFd, &s[0], count, false);
    return s;
  }

  void simulateTerminalResponse(const string& s) {
    lock_guard<recursive_mutex> lock(_mutex);
    socketHandler->writeAllOrThrow(serverClientFd, s.c_str(), s.length(),
                                   false);
  }
  virtual void handleSessionEnd() { didHandleSessionEnd = true; }
  virtual void cleanup() {
    lock_guard<recursive_mutex> lock(_mutex);
    if (didCleanUp) {
      return;
    }
    if (clientServerFd >= 0) {
      socketHandler->close(clientServerFd);
      clientServerFd = -1;
    }
    if (serverClientFd >= 0) {
      socketHandler->close(serverClientFd);
      serverClientFd = -1;
    }
    if (!pipePath.empty()) {
      SocketEndpoint endpoint;
      endpoint.set_name(pipePath);
      socketHandler->stopListening(endpoint);
    }
#ifndef WIN32
    if (!pipeDirectory.empty()) {
      FATAL_FAIL(::remove(pipeDirectory.c_str()));
    }
#endif
    didCleanUp = true;
  }
  virtual void setInfo(const winsize& tmpwin) {
    lock_guard<recursive_mutex> lock(terminalInfoMutex);
    lastWinInfo = tmpwin;
    setInfoCount++;
  }

  int getSetInfoCount() {
    lock_guard<recursive_mutex> lock(terminalInfoMutex);
    return setInfoCount;
  }

  winsize getLastWinInfo() {
    lock_guard<recursive_mutex> lock(terminalInfoMutex);
    return lastWinInfo;
  }

 protected:
  recursive_mutex _mutex;
  // Keep geometry updates independent from blocking socket I/O under _mutex.
  recursive_mutex terminalInfoMutex;
  shared_ptr<PipeSocketHandler> socketHandler;
  int serverClientFd;
  int clientServerFd;
  string pipeDirectory;
  string pipePath;
  int setInfoCount;
  bool didCleanUp;
  bool didHandleSessionEnd;
  winsize lastWinInfo;
};
}  // namespace et

#endif
