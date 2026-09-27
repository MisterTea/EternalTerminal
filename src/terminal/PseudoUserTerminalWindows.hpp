#ifndef __PSUEDO_USER_TERMINAL_WINDOWS_HPP__
#define __PSUEDO_USER_TERMINAL_WINDOWS_HPP__

#ifdef WIN32
#include <windows.h>

#include <atomic>
#include <condition_variable>
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

// Job termination is asynchronous and the job is optional, so fall back to
// terminating the root process directly.
inline bool TerminateProcessWithFallbackLocal(HANDLE process, HANDLE job) {
  bool terminatedByJob = false;
  if (job != nullptr) {
    terminatedByJob = TerminateJobObject(job, 1) != FALSE;
  }
  if (!terminatedByJob) {
    TerminateProcess(process, 1);
  }

  DWORD waitResult = WaitForSingleObject(process, 2000);
  if (waitResult != WAIT_OBJECT_0) {
    TerminateProcess(process, 1);
    waitResult = WaitForSingleObject(process, 2000);
  }
  return waitResult == WAIT_OBJECT_0;
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
        jobHandle(nullptr),
        closing(false) {}
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
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
            CREATE_SUSPENDED,
        NULL, wideHome.empty() ? NULL : wideHome.c_str(), &si.StartupInfo, &pi);

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

    // A job lets cleanup reach children that outlive the shell process.
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job == NULL) {
      LOG(WARNING) << "CreateJobObject failed; using direct-process cleanup: "
                   << GetLastError();
    } else {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {};
      jobInfo.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &jobInfo, sizeof(jobInfo)) ||
          !AssignProcessToJobObject(job, pi.hProcess)) {
        const DWORD error = GetLastError();
        LOG(WARNING) << "Configuring terminal process job failed; using "
                        "direct-process cleanup: "
                     << error;
        CloseHandle(job);
        job = nullptr;
      }
    }

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
      const DWORD error = GetLastError();
      TerminateProcessWithFallbackLocal(pi.hProcess, job);
      if (job != nullptr) {
        CloseHandle(job);
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      ClosePseudoConsole(pc);
      CloseHandle(ptyIn);
      CloseHandle(ptyOut);
      CloseHandle(ourIn);
      CloseHandle(ourOut);
      LOG(FATAL) << "Resuming terminal process failed: " << error;
    }

    CloseHandle(pi.hThread);
    CloseHandle(ptyIn);
    CloseHandle(ptyOut);
    hPC = pc;
    inputWrite = ourIn;
    outputRead = ourOut;
    processHandle = pi.hProcess;
    jobHandle = job;
    outputThread = std::thread([this]() {
      char bytes[16 * 1024];
      DWORD count = 0;
      HANDLE output = static_cast<HANDLE>(outputRead);
      // Runs until ClosePseudoConsole releases the pipe; the cap is lifted
      // while closing so the host can always flush.
      while (true) {
        {
          std::unique_lock<std::mutex> guard(pendingMutex);
          pendingDrained.wait(guard, [this]() {
            return closing || pendingOutput.size() < MAX_PENDING_OUTPUT;
          });
        }
        if (!ReadFile(output, bytes, sizeof(bytes), &count, NULL) ||
            count == 0) {
          break;
        }
        std::lock_guard<std::mutex> guard(pendingMutex);
        // The process tree is already terminated, so this is bounded; keep
        // queued output for the router.
        pendingOutput.append(bytes, count);
      }
    });
    VLOG(1) << "ConPTY opened for " << shell;
    // No pollable fd on Windows; the handler drains via drainOutput().
    return 0;
  }

  virtual void runTerminal() override {}

  virtual void cleanup() override {
    // Stop the producers before lifting the output cap.
    if (inputWrite != INVALID_HANDLE_VALUE) {
      CloseHandle(static_cast<HANDLE>(inputWrite));
      inputWrite = INVALID_HANDLE_VALUE;
    }
    if (processHandle != INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(static_cast<HANDLE>(processHandle), 0) !=
          WAIT_OBJECT_0) {
        TerminateProcessWithFallbackLocal(static_cast<HANDLE>(processHandle),
                                          static_cast<HANDLE>(jobHandle));
      }
      CloseHandle(static_cast<HANDLE>(processHandle));
      processHandle = INVALID_HANDLE_VALUE;
    }
    if (jobHandle != nullptr) {
      // KILL_ON_JOB_CLOSE also removes descendants that outlived the shell.
      CloseHandle(static_cast<HANDLE>(jobHandle));
      jobHandle = nullptr;
    }
    // Wake a reader parked at the cap: ClosePseudoConsole can wait on a host
    // that is blocked writing to a full pipe.
    closing = true;
    pendingDrained.notify_all();
    if (hPC != nullptr) {
      // Blocks until the host exits; the uncapped reader keeps it flowing.
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

  virtual int handleSessionEnd() override {
    int exitCode = 0;
    if (processHandle != INVALID_HANDLE_VALUE) {
      WaitForSingleObject(static_cast<HANDLE>(processHandle), INFINITE);
      DWORD code = 0;
      if (GetExitCodeProcess(static_cast<HANDLE>(processHandle), &code)) {
        exitCode = static_cast<int>(code);
      }
    }
    return exitCode;
  }

  virtual void terminate() override {
    if (jobHandle != nullptr &&
        !TerminateJobObject(static_cast<HANDLE>(jobHandle), 1)) {
      LOG(WARNING) << "Terminating terminal process tree failed: "
                   << GetLastError();
      if (processHandle != INVALID_HANDLE_VALUE) {
        TerminateProcess(static_cast<HANDLE>(processHandle), 1);
      }
    } else if (jobHandle == nullptr && processHandle != INVALID_HANDLE_VALUE) {
      TerminateProcess(static_cast<HANDLE>(processHandle), 1);
    }
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
    std::string out;
    {
      std::lock_guard<std::mutex> guard(pendingMutex);
      out.swap(pendingOutput);
    }
    pendingDrained.notify_one();
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
  static constexpr size_t MAX_PENDING_OUTPUT = 256 * 1024;
  void* hPC;
  void* inputWrite;
  void* outputRead;
  void* processHandle;
  void* jobHandle;
  std::thread outputThread;
  std::mutex pendingMutex;
  std::condition_variable pendingDrained;
  std::string pendingOutput;
  std::atomic<bool> closing;
};
}  // namespace et
#endif  // WIN32
#endif  // __PSUEDO_USER_TERMINAL_WINDOWS_HPP__
