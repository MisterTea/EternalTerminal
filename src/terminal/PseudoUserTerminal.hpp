#ifndef __PSUEDO_USER_TERMINAL_HPP__
#define __PSUEDO_USER_TERMINAL_HPP__

#ifdef WIN32
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "UserTerminal.hpp"

namespace et {

namespace {
inline std::wstring Utf8ToWideLocal(const std::string& s) {
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

inline std::string DefaultWindowsShellLocal() {
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

inline std::string DefaultWindowsHomeLocal() {
  const char* home = ::getenv("USERPROFILE");
  if (home && home[0]) {
    return std::string(home);
  }
  return std::string();
}
}  // namespace

/**
 * @brief ConPTY-backed terminal for Windows etterminal.
 *
 * Mirrors the Unix forkpty terminal but uses CreatePseudoConsole +
 * CreateProcess. Output is drained by a background thread into pendingOutput;
 * the Windows UserTerminalHandler pumps it to the router as raw bytes (same
 * wire protocol as Unix) and forwards router packets via writeInput()/setInfo.
 */
class PseudoUserTerminal : public UserTerminal {
 public:
  PseudoUserTerminal()
      : hPC(nullptr),
        inputWrite(INVALID_HANDLE_VALUE),
        outputRead(INVALID_HANDLE_VALUE),
        processHandle(INVALID_HANDLE_VALUE),
        running(false) {}
  virtual ~PseudoUserTerminal() { cleanup(); }

  virtual int setup(int /*routerFd*/) override {
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

    std::string shell = DefaultWindowsShellLocal();
    std::wstring wideShell = Utf8ToWideLocal(shell);
    std::wstring cmdLine = L"\"" + wideShell + L"\"";
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    std::string home = DefaultWindowsHomeLocal();
    std::wstring wideHome = Utf8ToWideLocal(home);
    // _putenv updates the CRT environment as well as the OS block.
    std::string etVersion = std::string("ET_VERSION=") + ET_VERSION;
    _putenv(etVersion.c_str());

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
      LOG(FATAL) << "CreateProcess for etterminal failed: " << GetLastError();
    }

    CloseHandle(pi.hThread);
    CloseHandle(ptyIn);
    CloseHandle(ptyOut);
    hPC = pc;
    inputWrite = ourIn;
    outputRead = ourOut;
    processHandle = pi.hProcess;
    running = true;
    outputThread = std::thread([this]() {
      char bytes[16 * 1024];
      DWORD count = 0;
      HANDLE output = static_cast<HANDLE>(outputRead);
      while (ReadFile(output, bytes, sizeof(bytes), &count, NULL) &&
             count > 0) {
        std::lock_guard<std::mutex> guard(pendingMutex);
        pendingOutput.append(bytes, count);
      }
      running = false;
    });
    VLOG(1) << "ConPTY opened for " << shell;
    // No pollable fd on Windows; the handler drains via drainOutput().
    return 0;
  }

  virtual void runTerminal() override {}

  virtual void cleanup() override {
    bool wasRunning = running.exchange(false);
    (void)wasRunning;
    if (inputWrite != INVALID_HANDLE_VALUE) {
      CloseHandle(static_cast<HANDLE>(inputWrite));
      inputWrite = INVALID_HANDLE_VALUE;
    }
    if (processHandle != INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(static_cast<HANDLE>(processHandle), 0) !=
          WAIT_OBJECT_0) {
        TerminateProcess(static_cast<HANDLE>(processHandle), 1);
        WaitForSingleObject(static_cast<HANDLE>(processHandle), 2000);
      }
      CloseHandle(static_cast<HANDLE>(processHandle));
      processHandle = INVALID_HANDLE_VALUE;
    }
    if (hPC != nullptr) {
      ClosePseudoConsole(static_cast<HPCON>(hPC));
      hPC = nullptr;
    }
    // Closing the pseudoconsole releases the output pipe, so the drain
    // thread's blocking ReadFile returns and the join below completes
    // (same order as htm TerminalHandler::stop()).
    if (outputThread.joinable()) {
      outputThread.join();
    }
    if (outputRead != INVALID_HANDLE_VALUE) {
      CloseHandle(static_cast<HANDLE>(outputRead));
      outputRead = INVALID_HANDLE_VALUE;
    }
  }

  virtual void handleSessionEnd() override {
    if (processHandle != INVALID_HANDLE_VALUE) {
      WaitForSingleObject(static_cast<HANDLE>(processHandle), INFINITE);
    }
    running = false;
  }

  virtual void setInfo(const winsize& tmpwin) override {
    if (hPC == nullptr) {
      return;
    }
    COORD size;
    size.X = static_cast<SHORT>(tmpwin.ws_col > 0 ? tmpwin.ws_col : 80);
    size.Y = static_cast<SHORT>(tmpwin.ws_row > 0 ? tmpwin.ws_row : 24);
    ResizePseudoConsole(static_cast<HPCON>(hPC), size);
  }

  virtual int getFd() override { return -1; }

  /** @brief Drains bytes collected by the ConPTY reader thread. */
  std::string drainOutput() {
    std::lock_guard<std::mutex> guard(pendingMutex);
    std::string out;
    out.swap(pendingOutput);
    return out;
  }

  /** @brief Writes client input bytes into the ConPTY. */
  void writeInput(const std::string& data) {
    if (inputWrite == INVALID_HANDLE_VALUE || data.empty()) {
      return;
    }
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(inputWrite), data.data(),
                   static_cast<DWORD>(data.size()), &written, NULL)) {
      LOG(WARNING) << "Writing terminal input failed: " << GetLastError();
    }
  }

  /** @brief True while the shell process is still alive. */
  bool isRunning() const {
    if (processHandle == INVALID_HANDLE_VALUE) {
      return false;
    }
    return WaitForSingleObject(static_cast<HANDLE>(processHandle), 0) !=
           WAIT_OBJECT_0;
  }

 protected:
  void* hPC;
  void* inputWrite;
  void* outputRead;
  void* processHandle;
  std::thread outputThread;
  std::mutex pendingMutex;
  std::string pendingOutput;
  std::atomic<bool> running;
};
}  // namespace et

#else  // !WIN32

#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#if __APPLE__
#include <sys/ucred.h>
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#elif __NetBSD__  // do not need pty.h on NetBSD
#else
#include <pty.h>
#endif

#ifdef WITH_UTEMPTER
#include <utempter.h>
#endif

#include "UserTerminal.hpp"

namespace et {
/**
 * @brief Forks a pseudo-terminal, runs the user's shell, and proxies the fd.
 */
class PseudoUserTerminal : public UserTerminal {
 public:
  virtual ~PseudoUserTerminal() {}

  virtual int setup(int routerFd) {
    pid_t pid = forkpty(&masterFd, NULL, NULL, NULL);
    switch (pid) {
      case -1:
        FATAL_FAIL(pid);
        break;
      case 0: {
        close(routerFd);
        runTerminal();
        // only get here if execl fails so a break is not needed since we exit
        exit(0);
      }
      default: {
        // parent
      }
    }

#ifdef WITH_UTEMPTER
    {
      char buf[1024];
      sprintf(buf, "etterminal [%lld]", (long long)getpid());
      utempter_add_record(masterFd, buf);
    }
#endif

    // The handler polls this fd with select() and does non-blocking reads and
    // writes, so make the master non-blocking here, where it is created.  If it
    // stayed blocking, a large input burst would block the handler's single
    // write() call and deadlock against the shell's echo (see
    // UserTerminal::setup and UserTerminalHandler::runUserTerminal).
    int flags = fcntl(masterFd, F_GETFL, 0);
    if (flags != -1) {
      fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
    }
    return masterFd;
  }

  /**
   * @brief Executes the login shell after setting up the PTY child process.
   */
  virtual void runTerminal() {
    passwd* pwd = getpwuid(getuid());
    chdir(pwd->pw_dir);
    string terminal = string(::getenv("SHELL"));
    VLOG(1) << "Child process launching terminal " << terminal;
    setenv("ET_VERSION", ET_VERSION, 1);
    // bash will not reset SIGCHLD to SIG_DFL when run, remembering the current
    // SIGCHLD disposition as the "original value" and allowing the user to
    // "reset" the signal handler to it's "original value" (trap --help).
    //
    // If our current SIGCHLD is SIG_IGN then it will be impossible, from
    // within bash, to set it to SIG_DFL by issuing "trap -- - SIGCHLD". This
    // in turn means that innocent implementations assuming they receive
    // SIGCHLD without anything special required on their part, break.
    // An example is Python2's popen(), which will fail with
    // "IOError: [Errno 10] No child processes".
    //
    // Such processes *could* help themselves by setting SIGCHLD to SIG_DFL
    // from within the process, but this is an esoteric requirement from the
    // process and many don't. And as mentioned, the shell user can't help
    // with "trap -- - SIGCHLD" either.
    //
    // Let's help everyone by setting SIGCHLD to SIG_DFL here, right before
    // exec'ing the shell. By doing it here, and not somewhere before, we add
    // no requirements for any wait(2) on our part.
    //
    signal(SIGCHLD, SIG_DFL);
    FATAL_FAIL(execl(terminal.c_str(), terminal.c_str(), "-l", NULL));
  }

  /** @brief Removes any temporary PTY bookkeeping (utempter). */
  virtual void cleanup() {
#ifdef WITH_UTEMPTER
    utempter_remove_record(masterFd);
#endif
  }

  /** @brief Waits for the child shell to exit before returning. */
  virtual void handleSessionEnd() {
#if __NetBSD__  // this unfortunateness seems to be fixed in NetBSD-8 (or at
                // least -CURRENT) sadness for now :/
    int throwaway;
    FATAL_FAIL(waitpid(getPid(), &throwaway, WUNTRACED));
#else
    siginfo_t childInfo;
    if (getPid() > 0) {
      if (waitid(P_PID, getPid(), &childInfo, WEXITED) == -1) {
        LOG(ERROR) << "waitid failed, child already reaped.";
      }
    }
#endif
  }

  /**
   * @brief Applies terminal resize changes via `ioctl(TIOCSWINSZ)`.
   */
  virtual void setInfo(const winsize& tmpwin) {
    ioctl(masterFd, TIOCSWINSZ, &tmpwin);
  }

  pid_t getPid() { return pid; }

  virtual int getFd() { return masterFd; }

 protected:
  /** @brief PID of the child shell spawned by `forkpty`. */
  pid_t pid;
  /** @brief Master PTY file descriptor shared with the router. */
  int masterFd;
};
}  // namespace et

#endif  // WIN32

#endif
