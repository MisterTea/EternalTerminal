#include <cxxopts.hpp>

#include "DaemonCreator.hpp"
#include "HtmClient.hpp"
#include "HtmServer.hpp"
#include "IpcPairClient.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "PipeSocketHandler.hpp"
#include "PseudoTerminalConsole.hpp"
#include "RawSocketUtils.hpp"
#include "SubprocessUtils.hpp"
#include "WinsockContext.hpp"

#ifdef WIN32
#include <tlhelp32.h>
#include <windows.h>
#else
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <limits.h>
#include <unistd.h>
#endif

using namespace et;

namespace {
unique_ptr<PseudoTerminalConsole> gConsole;

#ifndef WIN32
string siblingHtmdCommand() {
  char buf[PATH_MAX];
  buf[0] = '\0';
#ifdef __APPLE__
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) {
    return "htmd";
  }
#else
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) {
    return "htmd";
  }
  buf[n] = '\0';
#endif
  char resolved[PATH_MAX];
  if (!realpath(buf, resolved)) {
    return "htmd";
  }
  fs::path exe(resolved);
  fs::path htmd = exe.parent_path() / "htmd";
  if (!fs::exists(htmd)) {
    return "htmd";
  }
  return string("\"") + htmd.string() + "\"";
}
#endif

void writeHtmExitSequence() {
  char buf[] = {
      0x1b, 0x5b, '$', '$', '$', 'q',
  };
#ifdef WIN32
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, sizeof(buf), &written, NULL);
#else
  RawSocketUtils::writeAll(STDOUT_FILENO, buf, sizeof(buf));
#endif
  fflush(stdout);
}

void restoreTerminal() {
  if (gConsole) {
    gConsole->teardown();
  }
}

#ifdef WIN32
BOOL WINAPI consoleCtrlHandler(DWORD) {
  writeHtmExitSequence();
  restoreTerminal();
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
#else
void term(int) {
  writeHtmExitSequence();
  restoreTerminal();
  exit(1);
}
#endif
}  // namespace

int main(int argc, char** argv) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);
#ifdef WIN32
  WinsockContext winsockContext;
  if (!SetCurrentDirectoryA(GetTempDirectory().c_str())) {
    STFATAL << "Failed to use the temp directory for HTM IPC: "
            << GetLastError();
  }
#endif
  // Parse command line arguments
  cxxopts::Options options("htm", "Headless terminal multiplexer");
  options.allow_unrecognised_options();

  options.add_options()       //
      ("help", "Print help")  //
      ("x,kill-other-sessions",
       "kill all old sessions belonging to the user")  //
      ;

  auto result = options.parse(argc, argv);
  if (result.count("help")) {
    CLOG(INFO, "stdout") << options.help({}) << endl;
    exit(0);
  }

  setvbuf(stdin, NULL, _IONBF, 0);   // turn off buffering
  setvbuf(stdout, NULL, _IONBF, 0);  // turn off buffering

  gConsole.reset(new PseudoTerminalConsole());
  gConsole->setup();

#ifdef WIN32
  SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));
  action.sa_handler = term;
  sigaction(SIGTERM, &action, NULL);
#endif

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  el::Loggers::setVerboseLevel(3);
  LogHandler::setupLogFiles(&defaultConf, GetTempDirectory(), "htm", false,
                            true);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  et::HandleTerminate();

  // Override easylogging handler for sigint
  ::signal(SIGINT, et::InterruptSignalHandler);

#ifdef WIN32
  if (result.count("x")) {
    LOG(INFO) << "Killing previous htmd";
    killHtmdProcesses();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (!htmdProcessRunning()) {
    DaemonCreator::create(false, "");
  }
#else
  uid_t myuid = getuid();
  const string pipeName = HtmServer::getPipeName();
  auto htmdPids = [&]() {
    string command =
        string("pgrep -x -U ") + to_string(myuid) + string(" htmd");
    return SystemToStr(command.c_str());
  };
  if (result.count("x")) {
    LOG(INFO) << "Killing previous htmd";
    string command =
        string("pkill -x -U ") + to_string(myuid) + string(" htmd");
    system(command.c_str());
    for (int i = 0; i < 50; i++) {
      if (htmdPids().empty()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::unlink(pipeName.c_str());
  }

  if (htmdPids().empty()) {
    int daemonResult = DaemonCreator::create(false, "");
    if (daemonResult == DaemonCreator::CHILD) {
      exit(system(siblingHtmdCommand().c_str()));
    }
  }

  for (int i = 0; i < 100; i++) {
    if (!htmdPids().empty() && ::access(pipeName.c_str(), F_OK) == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#endif

  shared_ptr<SocketHandler> socketHandler(new PipeSocketHandler());
  SocketEndpoint pipeEndpoint;
  pipeEndpoint.set_name(HtmServer::getPipeName());
  try {
    HtmClient htmClient(socketHandler, pipeEndpoint);
    htmClient.run();
  } catch (const std::exception& ex) {
    LOG(ERROR) << "htm client exiting: " << ex.what();
    writeHtmExitSequence();
    restoreTerminal();
    return 1;
  }

  writeHtmExitSequence();
  restoreTerminal();

  return 0;
}
