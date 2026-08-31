#include "TerminalHandler.hpp"

#include "RawSocketUtils.hpp"

#ifdef WIN32
#include <windows.h>
#else
#include <chrono>

#include "ETerminal.pb.h"
#endif

namespace et {
#ifdef WIN32
namespace {
wstring utf8ToWide(const string& s) {
  if (s.empty()) {
    return wstring();
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
  wstring out(n ? n - 1 : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
  }
  return out;
}

string defaultWindowsShell() {
  const char* shell = ::getenv("SHELL");
  if (shell && shell[0]) {
    return string(shell);
  }
  shell = ::getenv("COMSPEC");
  if (shell && shell[0]) {
    return string(shell);
  }
  return string("cmd.exe");
}

string defaultWindowsHome() {
  const char* home = ::getenv("USERPROFILE");
  if (home && home[0]) {
    return string(home);
  }
  return string();
}
}  // namespace
#endif

TerminalHandler::TerminalHandler()
#ifdef WIN32
    : hPC(nullptr),
      inputWrite(INVALID_HANDLE_VALUE),
      outputRead(INVALID_HANDLE_VALUE),
      processHandle(INVALID_HANDLE_VALUE),
      run(false),
      bufferLength(0) {
}
#else
    : masterFd(-1), childPid(-1), run(false), bufferLength(0) {
}
#endif

TerminalHandler::~TerminalHandler() { stop(); }

#define MAX_BUFFER_LINES (1024)
#define MAX_BUFFER_CHARS (128 * MAX_BUFFER_LINES)

string TerminalHandler::bufferOutput(const string& newChars) {
  vector<string> tokens = split(newChars, '\n');
  for (auto& it : tokens) {
    bufferLength += it.length();
  }
  if (buffer.empty()) {
    buffer.insert(buffer.end(), tokens.begin(), tokens.end());
  } else {
    buffer.back().append(tokens.front());
    if (tokens.size() > 1) {
      buffer.insert(buffer.end(), tokens.begin() + 1, tokens.end());
    }
  }
  if (buffer.size() > MAX_BUFFER_LINES) {
    int amountToErase = buffer.size() - MAX_BUFFER_LINES;
    for (auto it = buffer.begin();
         it != buffer.end() && it != (buffer.begin() + amountToErase); it++) {
      bufferLength -= it->length();
    }
    buffer.erase(buffer.begin(), buffer.begin() + amountToErase);
  }
  while (bufferLength > MAX_BUFFER_CHARS) {
    bufferLength -= buffer.begin()->length();
    buffer.pop_front();
  }
  LOG(INFO) << "BUFFER LINES: " << buffer.size() << " " << tokens.size()
            << endl;
  return newChars;
}

#ifdef WIN32
void TerminalHandler::start() {
  HANDLE ptyIn = INVALID_HANDLE_VALUE;
  HANDLE ptyOut = INVALID_HANDLE_VALUE;
  HANDLE ourIn = INVALID_HANDLE_VALUE;
  HANDLE ourOut = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&ptyIn, &ourIn, NULL, 0) ||
      !CreatePipe(&ourOut, &ptyOut, NULL, 0)) {
    LOG(FATAL) << "CreatePipe failed: " << GetLastError();
  }

  COORD size;
  size.X = 80;
  size.Y = 24;
  HPCON pc = nullptr;
  HRESULT hr = CreatePseudoConsole(size, ptyIn, ptyOut, 0, &pc);
  CloseHandle(ptyIn);
  CloseHandle(ptyOut);
  if (FAILED(hr)) {
    CloseHandle(ourIn);
    CloseHandle(ourOut);
    LOG(FATAL) << "CreatePseudoConsole failed: " << hr;
  }

  SIZE_T attrBytes = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attrBytes);
  auto attrList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(
          GetProcessHeap(), 0, attrBytes));
  if (!attrList ||
      !InitializeProcThreadAttributeList(attrList, 1, 0, &attrBytes) ||
      !UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                 pc, sizeof(pc), NULL, NULL)) {
    if (attrList) {
      DeleteProcThreadAttributeList(attrList);
      HeapFree(GetProcessHeap(), 0, attrList);
    }
    ClosePseudoConsole(pc);
    CloseHandle(ourIn);
    CloseHandle(ourOut);
    LOG(FATAL) << "ProcThreadAttribute setup failed: " << GetLastError();
  }

  STARTUPINFOEXW si;
  ZeroMemory(&si, sizeof(si));
  si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  si.lpAttributeList = attrList;

  string shell = defaultWindowsShell();
  wstring wideShell = utf8ToWide(shell);
  wstring cmdLine = L"\"" + wideShell + L"\"";
  vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
  cmdBuf.push_back(L'\0');

  string home = defaultWindowsHome();
  wstring wideHome = utf8ToWide(home);

  SetEnvironmentVariableA("HTM_VERSION", ET_VERSION);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  BOOL ok = CreateProcessW(
      NULL, cmdBuf.data(), NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT,
      NULL, wideHome.empty() ? NULL : wideHome.c_str(), &si.StartupInfo, &pi);

  DeleteProcThreadAttributeList(attrList);
  HeapFree(GetProcessHeap(), 0, attrList);

  if (!ok) {
    ClosePseudoConsole(pc);
    CloseHandle(ourIn);
    CloseHandle(ourOut);
    LOG(FATAL) << "CreateProcess for HTM pane failed: " << GetLastError();
  }

  CloseHandle(pi.hThread);
  hPC = pc;
  inputWrite = ourIn;
  outputRead = ourOut;
  processHandle = pi.hProcess;
  run = true;
  VLOG(1) << "ConPTY opened for " << shell << endl;
}

string TerminalHandler::pollUserTerminal() {
  if (!run || outputRead == INVALID_HANDLE_VALUE) {
    return string();
  }

  if (processHandle != INVALID_HANDLE_VALUE &&
      WaitForSingleObject(static_cast<HANDLE>(processHandle), 0) ==
          WAIT_OBJECT_0) {
    LOG(INFO) << "Terminal session ended";
    run = false;
    return string();
  }

#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];
  HANDLE out = static_cast<HANDLE>(outputRead);
  DWORD avail = 0;
  if (!PeekNamedPipe(out, NULL, 0, NULL, &avail, NULL)) {
    LOG(INFO) << "Terminal session ended";
    run = false;
    return string();
  }
  if (avail == 0) {
    WaitForSingleObject(out, 10);
    if (!PeekNamedPipe(out, NULL, 0, NULL, &avail, NULL) || avail == 0) {
      return string();
    }
  }

  DWORD toRead = avail > BUF_SIZE ? BUF_SIZE : avail;
  DWORD n = 0;
  if (!ReadFile(out, b, toRead, &n, NULL)) {
    LOG(INFO) << "Terminal session ended";
    run = false;
    return string();
  }
  if (n == 0) {
    return string();
  }
  return bufferOutput(string(b, n));
}

void TerminalHandler::appendData(const string& data) {
  if (inputWrite == INVALID_HANDLE_VALUE || data.empty()) {
    return;
  }
  DWORD written = 0;
  WriteFile(static_cast<HANDLE>(inputWrite), data.data(),
            static_cast<DWORD>(data.size()), &written, NULL);
}

void TerminalHandler::updateTerminalSize(int col, int row) {
  if (hPC == nullptr) {
    return;
  }
  COORD size;
  size.X = static_cast<SHORT>(col > 0 ? col : 1);
  size.Y = static_cast<SHORT>(row > 0 ? row : 1);
  ResizePseudoConsole(static_cast<HPCON>(hPC), size);
}

void TerminalHandler::stop() {
  run = false;
  if (hPC != nullptr) {
    ClosePseudoConsole(static_cast<HPCON>(hPC));
    hPC = nullptr;
  }
  if (processHandle != INVALID_HANDLE_VALUE) {
    TerminateProcess(static_cast<HANDLE>(processHandle), 1);
    WaitForSingleObject(static_cast<HANDLE>(processHandle), 2000);
    CloseHandle(static_cast<HANDLE>(processHandle));
    processHandle = INVALID_HANDLE_VALUE;
  }
  if (inputWrite != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(inputWrite));
    inputWrite = INVALID_HANDLE_VALUE;
  }
  if (outputRead != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(outputRead));
    outputRead = INVALID_HANDLE_VALUE;
  }
}
#else
void TerminalHandler::start() {
  pid_t pid = forkpty(&masterFd, NULL, NULL, NULL);
  switch (pid) {
    case -1:
      FATAL_FAIL(pid);
    case 0: {
      passwd* pwd = getpwuid(getuid());
      if (pwd == NULL) {
        LOG(FATAL)
            << "Not able to fork a terminal because getpwuid returns null";
      }
      chdir(pwd->pw_dir);
      string terminal = string(::getenv("SHELL"));
      setenv("HTM_VERSION", ET_VERSION, 1);
      execl(terminal.c_str(), terminal.c_str(), "-l", NULL);
      exit(0);
      break;
    }
    default: {
      // parent
      VLOG(1) << "pty opened " << masterFd << endl;
      childPid = pid;
      run = true;
#ifdef WITH_UTEMPTER
      {
        char buf[1024];
        sprintf(buf, "htm [%lld]", (long long)getpid());
        utempter_add_record(masterFd, buf);
      }
#endif
      break;
    }
  }
}

string TerminalHandler::pollUserTerminal() {
  if (!run || masterFd < 0) {
    return string();
  }

#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  // Data structures needed for select() and
  // non-blocking I/O.
  fd_set rfd;
  timeval tv;

  FD_ZERO(&rfd);
  FD_SET(masterFd, &rfd);
  tv.tv_sec = 0;
  tv.tv_usec = 10000;
  select(masterFd + 1, &rfd, NULL, NULL, &tv);

  try {
    // Check for data to receive; the received
    // data includes also the data previously sent
    // on the same master descriptor (line 90).
    if (FD_ISSET(masterFd, &rfd)) {
      // Read from terminal and write to client
      memset(b, 0, BUF_SIZE);
      int rc = read(masterFd, b, BUF_SIZE);
      if (rc < 0) {
        // Terminal failed for some reason, bail.
        throw std::runtime_error("Terminal Failure");
      }
      if (rc > 0) {
        return bufferOutput(string(b, rc));
      } else {
        LOG(INFO) << "Terminal session ended";
#if __NetBSD__
        // this unfortunateness seems to be fixed in NetBSD-8 (or at
        // least -CURRENT) sadness for now :/
        int throwaway;
        FATAL_FAIL(waitpid(childPid, &throwaway, WUNTRACED));
#else
        siginfo_t childInfo;
        int rc = waitid(P_PID, childPid, &childInfo, WEXITED);
        if (rc < 0 && GetErrno() != ECHILD) {
          FATAL_FAIL(rc);
        }
#endif
        run = false;
#ifdef WITH_UTEMPTER
        utempter_remove_record(masterFd);
#endif
        return string();
      }
    }
  } catch (const std::exception& ex) {
    LOG(INFO) << ex.what();
    run = false;
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
  }

  return string();
}

void TerminalHandler::appendData(const string& data) {
  if (masterFd < 0 || data.empty()) {
    return;
  }
  RawSocketUtils::writeAll(masterFd, &data[0], data.length());
}

void TerminalHandler::updateTerminalSize(int col, int row) {
  if (masterFd < 0) {
    return;
  }
  winsize tmpwin;
  tmpwin.ws_row = row;
  tmpwin.ws_col = col;
  tmpwin.ws_xpixel = 0;
  tmpwin.ws_ypixel = 0;
  ioctl(masterFd, TIOCSWINSZ, &tmpwin);
}

void TerminalHandler::stop() {
  run = false;
  // Close the master PTY first so a child blocked on a full output buffer
  // is unblocked (SIGHUP/EIO) before we wait for it.
  if (masterFd >= 0) {
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
    close(masterFd);
    masterFd = -1;
  }
  if (childPid > 0) {
    kill(childPid, SIGKILL);
    int status = 0;
    for (int i = 0; i < 50; i++) {
      int rc = waitpid(childPid, &status, WNOHANG);
      if (rc == childPid || (rc < 0 && GetErrno() == ECHILD)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    childPid = -1;
  }
}
#endif

}  // namespace et
