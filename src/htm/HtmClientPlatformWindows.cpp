#include "ControlMode.hpp"
#include "DaemonCreator.hpp"
#include "HtmClientPlatform.hpp"
#include "HtmServer.hpp"

// clang-format off
#include <windows.h>
#include <tlhelp32.h>
// clang-format on

namespace et {
namespace htm_client_platform {
namespace {
void (*gRestoreTerminal)() = nullptr;

void writeHtmExitSequence() {
  const char* st = kControlModeSt;
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), st, static_cast<DWORD>(strlen(st)),
            &written, NULL);
}

BOOL WINAPI consoleCtrlHandler(DWORD) {
  writeHtmExitSequence();
  if (gRestoreTerminal) {
    gRestoreTerminal();
  }
  ExitProcess(1);
  return TRUE;
}

void killHtmdProcesses() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return;
  }
  PROCESSENTRY32W pe;
  ZeroMemory(&pe, sizeof(pe));
  pe.dwSize = sizeof(pe);
  DWORD self = GetCurrentProcessId();
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"htmd.exe") == 0 &&
          pe.th32ProcessID != self) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (h) {
          TerminateProcess(h, 1);
          CloseHandle(h);
        }
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
}

bool htmdProcessRunning() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return false;
  }
  PROCESSENTRY32W pe;
  ZeroMemory(&pe, sizeof(pe));
  pe.dwSize = sizeof(pe);
  bool found = false;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"htmd.exe") == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

bool requestHtmdShutdown() {
  HANDLE shutdownEvent = NULL;
  // htmd creates the event before entering its server loop. A just-spawned
  // daemon can therefore exist in the process table slightly before the event
  // is visible.
  for (int retry = 0; retry < 40 && !shutdownEvent; retry++) {
    shutdownEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE,
                               HtmServer::getShutdownEventName().c_str());
    if (!shutdownEvent) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (!shutdownEvent) {
    return false;
  }
  bool signaled = SetEvent(shutdownEvent) != FALSE;
  CloseHandle(shutdownEvent);
  if (!signaled) {
    return false;
  }
  for (int retry = 0; retry < 100; retry++) {
    if (!htmdProcessRunning()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}
}  // namespace

void installTerminationHandlers(void (*restoreTerminal)()) {
  gRestoreTerminal = restoreTerminal;
  SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
}

void ensureDaemon(bool killExisting) {
  if (killExisting) {
    LOG(INFO) << "Stopping previous htmd";
    if (htmdProcessRunning() && !requestHtmdShutdown()) {
      // Compatibility fallback for an older daemon that predates the graceful
      // shutdown event. New daemons must take the graceful path so Windows can
      // release the AF_UNIX pathname before the replacement starts.
      LOG(WARNING) << "Graceful htmd shutdown failed; terminating it";
      killHtmdProcesses();
    }
    for (int retry = 0; retry < 100 && htmdProcessRunning(); retry++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (!htmdProcessRunning()) {
    DaemonCreator::create(false, "");
  }
  const string pipeName = HtmServer::getPipeName();
  for (int retry = 0; retry < 100; retry++) {
    if (htmdProcessRunning() &&
        GetFileAttributesA(pipeName.c_str()) != INVALID_FILE_ATTRIBUTES) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

int finishClient(int code, void (*restoreTerminal)()) {
  writeHtmExitSequence();
  restoreTerminal();
  return code;
}
}  // namespace htm_client_platform
}  // namespace et
