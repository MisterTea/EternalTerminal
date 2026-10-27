#include "TerminalHandler.hpp"

// clang-format off
#include <windows.h>
#include <tlhelp32.h>
// clang-format on

namespace et {
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

string wideToUtf8(const wstring& w) {
  if (w.empty()) {
    return string();
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
  string out(n ? n : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], n, NULL, NULL);
    out.resize(n - 1);
  }
  return out;
}

string processImageBaseName(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return string();
  }
  wchar_t path[MAX_PATH];
  DWORD size = MAX_PATH;
  string name;
  if (QueryFullProcessImageNameW(process, 0, path, &size)) {
    wstring wide(path, size);
    auto slash = wide.find_last_of(L"\\/");
    wstring base = slash == wstring::npos ? wide : wide.substr(slash + 1);
    if (base.size() > 4) {
      auto ext = base.substr(base.size() - 4);
      if (_wcsicmp(ext.c_str(), L".exe") == 0) {
        base.resize(base.size() - 4);
      }
    }
    name = wideToUtf8(base);
    for (char& c : name) {
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
  }
  CloseHandle(process);
  return name;
}

bool isShellLikeProcess(const string& name) {
  return name.empty() || name == "cmd" || name == "powershell" ||
         name == "pwsh" || name == "powershell_ise" || name == "conhost" ||
         name == "openconsole" || name == "wt" || name == "windowsterminal" ||
         name == "htmd" || name == "htm";
}

// ConPTY has no tcgetpgrp; walk the shell's process tree and prefer a
// non-shell leaf (timeout, ping, sleep, …) for automatic-rename.
string deepestForegroundCommand(DWORD rootPid) {
  if (rootPid == 0) {
    return string();
  }
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return processImageBaseName(rootPid);
  }
  unordered_map<DWORD, vector<DWORD>> children;
  PROCESSENTRY32W entry;
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snap, &entry)) {
    do {
      children[entry.th32ParentProcessID].push_back(entry.th32ProcessID);
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);

  string best;
  vector<DWORD> stack{rootPid};
  while (!stack.empty()) {
    DWORD pid = stack.back();
    stack.pop_back();
    const auto it = children.find(pid);
    if (it == children.end() || it->second.empty()) {
      string name = processImageBaseName(pid);
      if (!isShellLikeProcess(name)) {
        best = name;
      } else if (best.empty() && pid == rootPid) {
        best = name;
      }
      continue;
    }
    for (DWORD child : it->second) {
      stack.push_back(child);
    }
  }
  // Once a foreground child such as timeout.exe exits, the command shell is
  // again the active ConPTY process. Returning an empty string leaves the
  // previous automatic-rename title stuck on the exited child forever.
  // Reporting the root shell lets MultiplexerState publish the transition.
  return best.empty() ? processImageBaseName(rootPid) : best;
}
}  // namespace

TerminalHandler::TerminalHandler()
    : hPC(nullptr),
      inputWrite(INVALID_HANDLE_VALUE),
      outputRead(INVALID_HANDLE_VALUE),
      processHandle(INVALID_HANDLE_VALUE),
      run(false),
      bufferLength(0) {}

bool TerminalHandler::isRunning() {
  if (processHandle == INVALID_HANDLE_VALUE) {
    return false;
  }
  return WaitForSingleObject(static_cast<HANDLE>(processHandle), 0) !=
         WAIT_OBJECT_0;
}

int64_t TerminalHandler::childProcessId() const {
  if (processHandle == INVALID_HANDLE_VALUE) {
    return 0;
  }
  return static_cast<int64_t>(GetProcessId(static_cast<HANDLE>(processHandle)));
}

string TerminalHandler::foregroundCommand() const {
  if (processHandle == INVALID_HANDLE_VALUE) {
    return string();
  }
  const DWORD rootPid = GetProcessId(static_cast<HANDLE>(processHandle));
  return deepestForegroundCommand(rootPid);
}

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
}  // namespace et
