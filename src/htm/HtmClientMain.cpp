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
  if (result.count("x")) {
    LOG(INFO) << "Killing previous htmd";
    // Kill previous htm daemon
    string command =
        string("pkill -x -U ") + to_string(myuid) + string(" htmd");
    system(command.c_str());
  }

  // Check if daemon exists
  string command = string("pgrep -x -U ") + to_string(myuid) + string(" htmd");
  string pgrepOutput = SystemToStr(command.c_str());

  if (pgrepOutput.length() == 0) {
    // Fork to create the daemon
    int daemonResult = DaemonCreator::create(false, "");
    if (daemonResult == DaemonCreator::CHILD) {
      // This means we are the daemon
      exit(system(siblingHtmdCommand().c_str()));
    }
  }
#endif

  // This means we are the client to the daemon
  std::this_thread::sleep_for(std::chrono::microseconds(
      10 * 1000));  // Sleep for 10ms to let the daemon come alive
  shared_ptr<SocketHandler> socketHandler(new PipeSocketHandler());
  SocketEndpoint pipeEndpoint;
  pipeEndpoint.set_name(HtmServer::getPipeName());
  HtmClient htmClient(socketHandler, pipeEndpoint);
  htmClient.run();

  writeHtmExitSequence();
  restoreTerminal();

  return 0;
}
