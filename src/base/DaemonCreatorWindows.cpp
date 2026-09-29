#include <windows.h>

#include "DaemonCreator.hpp"

namespace et {
int DaemonCreator::createSessionLeader() { return 0; }

int DaemonCreator::create(bool parentExit, string childPidFile) {
  wchar_t modulePath[MAX_PATH];
  DWORD moduleLen = GetModuleFileNameW(NULL, modulePath, MAX_PATH);
  fs::path currentExe;
  wstring currentStem;
  if (moduleLen != 0) {
    currentExe = fs::path(modulePath);
    currentStem = currentExe.stem().wstring();
    for (auto& c : currentStem) {
      c = towlower(c);
    }
  }

  // htm.exe daemonizes htmd.exe (sibling or on PATH). Every other binary
  // (etserver.exe, etterminal.exe) daemonizes itself detached, mirroring the
  // Unix double-fork.
  bool launchHtmd = (currentStem == L"htm");
  fs::path htmdPath;
  if (launchHtmd && moduleLen != 0) {
    htmdPath = currentExe.parent_path() / L"htmd.exe";
  }

  wstring cmdLine;
  const wchar_t* application = nullptr;
  if (launchHtmd) {
    if (!htmdPath.empty() && fs::exists(htmdPath)) {
      application = htmdPath.c_str();
      cmdLine = L"\"" + htmdPath.wstring() + L"\"";
    } else {
      cmdLine = L"htmd.exe";
    }
  } else if (moduleLen != 0) {
    // Relaunch self detached with the same arguments (minus --daemon, which
    // the child does not need to re-handle).
    application = nullptr;  // Use command line's executable.
    wstring fullCmd = GetCommandLineW();
    // GetCommandLineW includes the exe; reuse it verbatim so all flags
    // survive. The child sees --daemon again but create() is idempotent: it
    // will spawn once more only if --daemon handling loops, so strip it.
    cmdLine = fullCmd;
  } else {
    cmdLine = L"htmd.exe";
  }

  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
  cmdBuf.push_back(L'\0');

  BOOL ok =
      CreateProcessW(application, cmdBuf.data(), NULL, NULL, FALSE,
                     DETACHED_PROCESS | CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  if (!ok) {
    STFATAL << "Failed to start htmd: " << GetLastError();
  }

  if (!childPidFile.empty()) {
    std::ofstream pidFile(childPidFile.c_str());
    if (pidFile) {
      pidFile << pi.dwProcessId << "\n";
    }
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  if (parentExit) {
    exit(EXIT_SUCCESS);
  }
  return PARENT;
}
}  // namespace et
