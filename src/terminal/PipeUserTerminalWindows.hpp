#ifndef __PIPE_USER_TERMINAL_WINDOWS_HPP__
#define __PIPE_USER_TERMINAL_WINDOWS_HPP__

#ifdef WIN32
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "UserTerminal.hpp"

namespace et {

namespace {
inline std::wstring PipeUtf8ToWide(const std::string& s) {
  if (s.empty()) {
    return std::wstring();
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
  std::wstring out(n ? n : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    out.resize(n - 1);
  }
  return out;
}

inline std::string PipeDefaultWindowsShell() {
  const char* shell = ::getenv("SHELL");
  if (shell && shell[0]) {
    return std::string(shell);
  }
  shell = ::getenv("COMSPEC");
  if (shell && shell[0]) {
    return std::string(shell);
  }
  return std::string("cmd.exe");
}
}  // namespace

/**
 * @brief Runs a remote command on anonymous pipes without a ConPTY.
 */
class PipeUserTerminal : public UserTerminal {
 public:
  explicit PipeUserTerminal(const string& command)
      : command(command),
        processHandle(INVALID_HANDLE_VALUE),
        stdinWrite(INVALID_HANDLE_VALUE),
        stdoutRead(INVALID_HANDLE_VALUE),
        stderrRead(INVALID_HANDLE_VALUE),
        stdoutSocket(-1),
        stderrSocket(-1),
        stdinSocket(-1),
        running(false) {}

  virtual ~PipeUserTerminal() { cleanup(); }

  virtual int setup(int /*routerFd*/) override {
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE childStdInRead = INVALID_HANDLE_VALUE;
    HANDLE childStdOutWrite = INVALID_HANDLE_VALUE;
    HANDLE childStdErrWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&childStdInRead, &stdinWrite, &saAttr, 0) ||
        !CreatePipe(&stdoutRead, &childStdOutWrite, &saAttr, 0) ||
        !CreatePipe(&stderrRead, &childStdErrWrite, &saAttr, 0)) {
      LOG(FATAL) << "CreatePipe failed: " << GetLastError();
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = childStdInRead;
    si.hStdOutput = childStdOutWrite;
    si.hStdError = childStdErrWrite;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    string shell = PipeDefaultWindowsShell();
    wstring wideShell = PipeUtf8ToWide(shell);
    wstring cmdline;
    // cmd.exe uses /c; posix-ish shells use -c.
    if (shell.find("cmd") != string::npos ||
        shell.find("CMD") != string::npos) {
      cmdline = L"\"" + wideShell + L"\" /c " + PipeUtf8ToWide(command);
    } else {
      cmdline = L"\"" + wideShell + L"\" -c " + PipeUtf8ToWide(command);
    }
    vector<wchar_t> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back(L'\0');

    BOOL ok = CreateProcessW(NULL, mutableCmd.data(), NULL, NULL, TRUE, 0, NULL,
                             NULL, &si, &pi);
    CloseHandle(childStdInRead);
    CloseHandle(childStdOutWrite);
    CloseHandle(childStdErrWrite);
    if (!ok) {
      LOG(FATAL) << "CreateProcess failed: " << GetLastError();
    }
    CloseHandle(pi.hThread);
    processHandle = pi.hProcess;
    running = true;

    // Bridge HANDLEs to socket fds so UserTerminalHandler can poll them.
    int stdoutPair[2] = {-1, -1};
    int stderrPair[2] = {-1, -1};
    int stdinPair[2] = {-1, -1};
    if (testCreateSocketPair(stdoutPair) != 0 ||
        testCreateSocketPair(stderrPair) != 0 ||
        testCreateSocketPair(stdinPair) != 0) {
      LOG(FATAL) << "Failed to create pipe bridge sockets";
    }
    stdoutSocket = stdoutPair[0];
    stderrSocket = stderrPair[0];
    stdinSocket = stdinPair[1];
    startBridgeThreads(stdoutPair[1], stderrPair[1], stdinPair[0]);
    return stdoutSocket;
  }

  virtual void runTerminal() override {}
  virtual void handleSessionEnd() override {
    if (processHandle != INVALID_HANDLE_VALUE) {
      WaitForSingleObject(processHandle, INFINITE);
    }
  }
  virtual void cleanup() override {
    running = false;
    if (stdoutBridge.joinable()) {
      stdoutBridge.join();
    }
    if (stderrBridge.joinable()) {
      stderrBridge.join();
    }
    if (stdinBridge.joinable()) {
      stdinBridge.join();
    }
    auto closeHandle = [](HANDLE& h) {
      if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
      }
    };
    closeHandle(stdinWrite);
    closeHandle(stdoutRead);
    closeHandle(stderrRead);
    closeHandle(processHandle);
    auto closeSock = [](int& fd) {
      if (fd >= 0) {
        closesocket(fd);
        fd = -1;
      }
    };
    closeSock(stdoutSocket);
    closeSock(stderrSocket);
    closeSock(stdinSocket);
  }
  virtual void setInfo(const winsize& /*tmpwin*/) override {}
  virtual int getFd() override { return stdoutSocket; }
  virtual int getInputFd() override { return stdinSocket; }
  virtual int getStderrFd() override { return stderrSocket; }

 protected:
  // Local helper so this header does not depend on test utilities.
  static int testCreateSocketPair(int fds[2]) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
      return -1;
    }
    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listener, 1) != 0) {
      closesocket(listener);
      return -1;
    }
    int addrLen = sizeof(addr);
    getsockname(listener, (sockaddr*)&addr, &addrLen);
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
      closesocket(listener);
      return -1;
    }
    if (connect(client, (sockaddr*)&addr, sizeof(addr)) != 0) {
      closesocket(client);
      closesocket(listener);
      return -1;
    }
    SOCKET server = accept(listener, NULL, NULL);
    closesocket(listener);
    if (server == INVALID_SOCKET) {
      closesocket(client);
      return -1;
    }
    fds[0] = (int)server;
    fds[1] = (int)client;
    return 0;
  }

  void startBridgeThreads(int stdoutWriteSock, int stderrWriteSock,
                          int stdinReadSock) {
    stdoutBridge = thread([this, stdoutWriteSock]() {
      char buf[4096];
      DWORD n = 0;
      while (running.load()) {
        if (!ReadFile(stdoutRead, buf, sizeof(buf), &n, NULL) || n == 0) {
          break;
        }
        int off = 0;
        while (off < (int)n) {
          int m = ::send(stdoutWriteSock, buf + off, n - off, 0);
          if (m <= 0) {
            break;
          }
          off += m;
        }
      }
      closesocket(stdoutWriteSock);
    });
    stderrBridge = thread([this, stderrWriteSock]() {
      char buf[4096];
      DWORD n = 0;
      while (running.load()) {
        if (!ReadFile(stderrRead, buf, sizeof(buf), &n, NULL) || n == 0) {
          break;
        }
        int off = 0;
        while (off < (int)n) {
          int m = ::send(stderrWriteSock, buf + off, n - off, 0);
          if (m <= 0) {
            break;
          }
          off += m;
        }
      }
      closesocket(stderrWriteSock);
    });
    stdinBridge = thread([this, stdinReadSock]() {
      char buf[4096];
      while (running.load()) {
        int n = ::recv(stdinReadSock, buf, sizeof(buf), 0);
        if (n <= 0) {
          break;
        }
        DWORD written = 0;
        int off = 0;
        while (off < n) {
          if (!WriteFile(stdinWrite, buf + off, n - off, &written, NULL)) {
            break;
          }
          off += (int)written;
        }
      }
      closesocket(stdinReadSock);
      if (stdinWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(stdinWrite);
        stdinWrite = INVALID_HANDLE_VALUE;
      }
    });
  }

  string command;
  HANDLE processHandle;
  HANDLE stdinWrite;
  HANDLE stdoutRead;
  HANDLE stderrRead;
  int stdoutSocket;
  int stderrSocket;
  int stdinSocket;
  std::atomic<bool> running;
  thread stdoutBridge;
  thread stderrBridge;
  thread stdinBridge;
};
}  // namespace et

#endif  // WIN32
#endif
