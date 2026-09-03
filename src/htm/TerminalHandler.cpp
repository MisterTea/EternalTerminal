#include "TerminalHandler.hpp"

#include "RawSocketUtils.hpp"

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#include <chrono>
#include <fstream>
#include <stdexcept>

#include "ETerminal.pb.h"

#ifdef CODE_COVERAGE
extern "C" void __gcov_reset(void);
#endif
#endif

namespace et {
#ifdef WIN32
namespace {
wstring utf8ToWide(const string& s) {
  if (s.empty()) {
    return wstring();
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
  wstring out(n ? n : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    out.resize(n - 1);
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
      bufferLength(0){}
#else
    : masterFd(-1), childPid(-1), run(false), bufferLength(0) {
}
#endif

      TerminalHandler::~TerminalHandler() {
  stop();
}

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
  VLOG(1) << "BUFFER LINES: " << buffer.size() << " " << tokens.size();
  return newChars;
}

int64_t TerminalHandler::childProcessId() const {
#ifdef WIN32
  if (processHandle == INVALID_HANDLE_VALUE) {
    return 0;
  }
  return static_cast<int64_t>(GetProcessId(static_cast<HANDLE>(processHandle)));
#else
  return childPid > 0 ? static_cast<int64_t>(childPid) : 0;
#endif
}

string TerminalHandler::foregroundCommand() const {
#ifdef WIN32
  return string();
#else
  auto name_of = [](pid_t pid) -> string {
    if (pid <= 0) {
      return string();
    }
    string comm;
#ifdef __APPLE__
    char name[128];
    if (proc_name(pid, name, sizeof(name)) > 0) {
      comm = string(name);
    }
#elif defined(__FreeBSD__)
    struct kinfo_proc kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t len = sizeof(kp);
    if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && len >= sizeof(kp)) {
      comm = string(kp.ki_comm);
    }
#else
    ifstream in(string("/proc/") + to_string(pid) + "/comm");
    getline(in, comm);
#endif
    if (comm == "pgrep" || comm == "pkill" || comm == "htmd" || comm == "htm") {
      return string();
    }
    return comm;
  };
  string comm;
  if (masterFd >= 0) {
    pid_t pgid = tcgetpgrp(masterFd);
    if (pgid > 0 && pgid != childPid) {
      comm = name_of(pgid);
    }
  }
  if (comm.empty()) {
    comm = name_of(childPid);
  }
  return comm;
#endif
}

#ifdef WIN32
void TerminalHandler::start(const string& cwd, int cols, int rows) {
  HANDLE ptyIn = INVALID_HANDLE_VALUE;
  HANDLE ptyOut = INVALID_HANDLE_VALUE;
  HANDLE ourIn = INVALID_HANDLE_VALUE;
  HANDLE ourOut = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&ptyIn, &ourIn, NULL, 0) ||
      !CreatePipe(&ourOut, &ptyOut, NULL, 0)) {
    LOG(FATAL) << "CreatePipe failed: " << GetLastError();
  }

  COORD size;
  size.X = static_cast<SHORT>(cols > 0 ? cols : 80);
  size.Y = static_cast<SHORT>(rows > 0 ? rows : 24);
  HPCON pc = nullptr;
  HRESULT hr = CreatePseudoConsole(size, ptyIn, ptyOut, 0, &pc);
  if (FAILED(hr)) {
    CloseHandle(ptyIn);
    CloseHandle(ptyOut);
    CloseHandle(ourIn);
    CloseHandle(ourOut);
    LOG(FATAL) << "CreatePseudoConsole failed: " << hr;
  }

  SIZE_T attrBytes = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attrBytes);
  auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
      HeapAlloc(GetProcessHeap(), 0, attrBytes));
  if (!attrList ||
      !InitializeProcThreadAttributeList(attrList, 1, 0, &attrBytes) ||
      !UpdateProcThreadAttribute(attrList, 0,
                                 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc,
                                 sizeof(pc), NULL, NULL)) {
    if (attrList) {
      DeleteProcThreadAttributeList(attrList);
      HeapFree(GetProcessHeap(), 0, attrList);
    }
    ClosePseudoConsole(pc);
    CloseHandle(ptyIn);
    CloseHandle(ptyOut);
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

  string home = cwd.empty() ? defaultWindowsHome() : cwd;
  wstring wideHome = utf8ToWide(home);

  SetEnvironmentVariableA("HTM_VERSION", ET_VERSION);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  BOOL ok = CreateProcessW(
      wideShell.c_str(), cmdBuf.data(), NULL, NULL, FALSE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, NULL,
      wideHome.empty() ? NULL : wideHome.c_str(), &si.StartupInfo, &pi);

  DeleteProcThreadAttributeList(attrList);
  HeapFree(GetProcessHeap(), 0, attrList);

  if (!ok) {
    ClosePseudoConsole(pc);
    CloseHandle(ptyIn);
    CloseHandle(ptyOut);
    CloseHandle(ourIn);
    CloseHandle(ourOut);
    LOG(FATAL) << "CreateProcess for HTM pane failed: " << GetLastError();
  }

  CloseHandle(pi.hThread);
  // The pseudoconsole owns duplicated copies after CreateProcess succeeds.
  CloseHandle(ptyIn);
  CloseHandle(ptyOut);
  hPC = pc;
  inputWrite = ourIn;
  outputRead = ourOut;
  processHandle = pi.hProcess;
  run = true;
  outputThread = thread([this]() {
    char bytes[16 * 1024];
    DWORD count = 0;
    HANDLE output = static_cast<HANDLE>(outputRead);
    while (ReadFile(output, bytes, sizeof(bytes), &count, NULL) && count > 0) {
      lock_guard<mutex> guard(pendingOutputMutex);
      pendingOutput.append(bytes, count);
    }
    run = false;
  });
  VLOG(1) << "ConPTY opened for " << shell << endl;
}

string TerminalHandler::pollUserTerminal() {
  string output;
  {
    lock_guard<mutex> guard(pendingOutputMutex);
    output.swap(pendingOutput);
  }
  if (!output.empty()) {
    return bufferOutput(output);
  }
  if (processHandle != INVALID_HANDLE_VALUE &&
      WaitForSingleObject(static_cast<HANDLE>(processHandle), 0) ==
          WAIT_OBJECT_0) {
    DWORD exitCode = 0;
    GetExitCodeProcess(static_cast<HANDLE>(processHandle), &exitCode);
    if (run.exchange(false)) {
      LOG(INFO) << "Terminal session ended with exit code " << exitCode;
    }
  }
  return string();
}

void TerminalHandler::appendData(const string& data) {
  if (inputWrite == INVALID_HANDLE_VALUE || data.empty()) {
    return;
  }
  DWORD written = 0;
  if (!WriteFile(static_cast<HANDLE>(inputWrite), data.data(),
                 static_cast<DWORD>(data.size()), &written, NULL)) {
    LOG(WARNING) << "Writing terminal input failed: " << GetLastError();
  } else if (written != data.size()) {
    LOG(WARNING) << "Only wrote " << written << " of " << data.size()
                 << " terminal input bytes";
  }
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
  if (inputWrite != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(inputWrite));
    inputWrite = INVALID_HANDLE_VALUE;
  }
  if (processHandle != INVALID_HANDLE_VALUE) {
    TerminateProcess(static_cast<HANDLE>(processHandle), 1);
    WaitForSingleObject(static_cast<HANDLE>(processHandle), 2000);
    CloseHandle(static_cast<HANDLE>(processHandle));
    processHandle = INVALID_HANDLE_VALUE;
  }
  if (hPC != nullptr) {
    ClosePseudoConsole(static_cast<HPCON>(hPC));
    hPC = nullptr;
  }
  if (outputThread.joinable()) {
    outputThread.join();
  }
  if (outputRead != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(outputRead));
    outputRead = INVALID_HANDLE_VALUE;
  }
}
#else
void TerminalHandler::start(const string& cwd, int cols, int rows) {
  winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
  ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
  pid_t pid = forkpty(&masterFd, NULL, NULL, &ws);
  switch (pid) {
    case -1:
      throw std::runtime_error(string("forkpty failed: ") +
                               strerror(GetErrno()));
    case 0: {
      passwd* pwd = getpwuid(getuid());
      if (pwd == NULL) {
        LOG(FATAL)
            << "Not able to fork a terminal because getpwuid returns null";
      }
      if (!cwd.empty()) {
        if (chdir(cwd.c_str()) != 0 && pwd->pw_dir) {
          chdir(pwd->pw_dir);
        }
      } else if (pwd->pw_dir) {
        chdir(pwd->pw_dir);
      }
      const char* shellEnv = ::getenv("SHELL");
      string terminal =
          (shellEnv && shellEnv[0]) ? string(shellEnv) : string("/bin/sh");
      setenv("HTM_VERSION", ET_VERSION, 1);
      // Match tmux -f /dev/null (default-terminal screen) so shells send the
      // same OSC/title sequences iTerm2 sees under tmux -CC.
      setenv("TERM", "screen", 1);
      // zsh's default PROMPT_EOL_MARK is a highlighted `%` plus spaces to the
      // right margin. GUI panes (Hyper, iTerm2) reflow that padding into a
      // stray `%` on its own line. Empty the mark so a fresh pane is clean.
      setenv("PROMPT_EOL_MARK", "", 1);
      // Non-login: inherit PATH from htmd and skip login scripts that may
      // switch to csh (FreeBSD's default user shell).
#ifdef CODE_COVERAGE
      // Drop inherited counters so the child does not dump .gcda on a failed
      // execl (exit/atexit) while the parent is still running under ctest.
      __gcov_reset();
#endif
      execl(terminal.c_str(), terminal.c_str(), NULL);
      _exit(127);
      break;
    }
    default: {
      // parent
      VLOG(1) << "pty opened " << masterFd << endl;
      childPid = pid;
      run = true;
      int flags = fcntl(masterFd, F_GETFL, 0);
      if (flags >= 0) {
        fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
      }
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
  flushPendingWrite();

#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  fd_set rfd;
  timeval tv;

  FD_ZERO(&rfd);
  FD_SET(masterFd, &rfd);
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  select(masterFd + 1, &rfd, NULL, NULL, &tv);

  string collected;
  bool hungup = false;
  try {
    if (FD_ISSET(masterFd, &rfd)) {
      while (true) {
        int rc = read(masterFd, b, BUF_SIZE);
        if (rc > 0) {
          collected.append(b, rc);
          continue;
        }
        if (rc < 0 && (GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK ||
                       GetErrno() == EINTR)) {
          break;
        }
        // rc == 0 (Linux EOF) or EIO (BSD): PTY slave closed.
        hungup = true;
        LOG(INFO) << "Terminal session ended";
        break;
      }
    }
  } catch (const std::exception& ex) {
    LOG(INFO) << ex.what();
    hungup = true;
  }

  if (childPid > 0) {
    int status = 0;
    int wr = waitpid(childPid, &status, WNOHANG);
    if (wr == childPid || (wr < 0 && GetErrno() == ECHILD)) {
      hungup = true;
      childPid = -1;
    }
  }

  if (hungup) {
    run = false;
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
  }

  if (!collected.empty()) {
    return bufferOutput(collected);
  }
  return string();
}

void TerminalHandler::flushPendingWrite() {
  if (masterFd < 0 || pendingWrite.empty()) {
    return;
  }
  while (!pendingWrite.empty()) {
    ssize_t rc = write(masterFd, pendingWrite.data(), pendingWrite.size());
    if (rc > 0) {
      pendingWrite.erase(0, static_cast<size_t>(rc));
      continue;
    }
    if (rc < 0 && (GetErrno() == EAGAIN || GetErrno() == EWOULDBLOCK ||
                   GetErrno() == EINTR)) {
      return;
    }
    LOG(INFO) << "Terminal write failed";
    run = false;
    pendingWrite.clear();
    return;
  }
}

void TerminalHandler::appendData(const string& data) {
  if (masterFd < 0 || data.empty()) {
    return;
  }
  pendingWrite.append(data);
  const size_t maxPending = 1024 * 1024;
  if (pendingWrite.size() > maxPending) {
    pendingWrite.erase(0, pendingWrite.size() - maxPending);
  }
  flushPendingWrite();
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
    pendingWrite.clear();
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
