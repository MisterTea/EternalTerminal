#include <cxxopts.hpp>

#include "DaemonCreator.hpp"
#include "HtmClient.hpp"
#include "HtmServer.hpp"
#include "IpcPairClient.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "PipeSocketHandler.hpp"
#include "RawSocketUtils.hpp"
#include "SubprocessToString.hpp"
#include "WinsockContext.hpp"

using namespace et;

#ifdef WIN32
bool IsProcessRunning(const TCHAR* const executableName) {
  PROCESSENTRY32 entry;
  entry.dwSize = sizeof(PROCESSENTRY32);

  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);

  if (!Process32First(snapshot, &entry)) {
    CloseHandle(snapshot);
    return false;
  }

  do {
    if (!_tcsicmp(entry.szExeFile, executableName)) {
      CloseHandle(snapshot);
      return true;
    }
  } while (Process32Next(snapshot, &entry));

  CloseHandle(snapshot);
  return false;
}
#else
termios terminal_backup;

void term(int signum) {
  char buf[] = {
      0x1b, 0x5b, '$', '$', '$', 'q',
  };
  RawSocketUtils::writeAll(STDOUT_FILENO, buf, sizeof(buf));
  fflush(stdout);
  tcsetattr(0, TCSANOW, &terminal_backup);
  exit(1);
}
#endif

int main(int argc, char** argv) {
  WinsockContext winsockContext;
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);
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

  // Turn on raw terminal mode
#ifdef WIN32
  DWORD inputMode;
  DWORD outputMode;
  auto hstdin = GetStdHandle(STD_INPUT_HANDLE);
  GetConsoleMode(hstdin, &inputMode);
  auto hstdout = GetStdHandle(STD_OUTPUT_HANDLE);
  GetConsoleMode(hstdout, &outputMode);

  SetConsoleMode(hstdin, ENABLE_VIRTUAL_TERMINAL_INPUT);
#else
  termios terminal_local;
  tcgetattr(0, &terminal_local);
  memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
  cfmakeraw(&terminal_local);
  tcsetattr(STDOUT_FILENO, TCSANOW, &terminal_local);
#endif

  // Catch sigterm and send exit control code
#ifdef WIN32
#else
  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));
  action.sa_handler = term;
  sigaction(SIGTERM, &action, NULL);
#endif

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
  el::Loggers::setVerboseLevel(3);
  // default max log file size is 20MB for etserver
  string maxlogsize = "20971520";
  LogHandler::setupLogFile(&defaultConf, GetTempDirectory() + "htm.log",
                           maxlogsize);
  // Redirect std streams to a file
  LogHandler::stderrToFile(GetTempDirectory() + "htm");

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  et::HandleTerminate();

  // Override easylogging handler for sigint
  ::signal(SIGINT, et::InterruptSignalHandler);

#ifdef WIN32
  auto username = GetOsUserName();
  string taskKillCommand = "taskkill /U " + username + " htmd.exe";
  system(taskKillCommand.c_str());
#else
  uid_t myuid = getuid();
  if (result.count("x")) {
    LOG(INFO) << "Killing previous htmd";
    // Kill previous htm daemon
    string taskKillCommand =
        string("pkill -x -U ") + to_string(myuid) + string(" htmd");
    system(taskKillCommand.c_str());
  }
#endif

  // Check if daemon exists
#ifdef WIN32
  bool daemonExists = IsProcessRunning(_TEXT("htmd.exe"));
#else
  string command = string("pgrep -x -U ") + GetOsUserName() + string(" htmd");
  string pgrepOutput = SystemToStr(command.c_str());
  bool daemonExists = pgrepOutput.length() > 0;
#endif

  if (!daemonExists) {
    // Fork to create the daemon
#ifdef WIN32
    system("start E:\\github\\EternalTerminal\\msvc_build\\Debug\\htmd.exe");
#else
    int result = DaemonCreator::create(false, "");
    if (result == DaemonCreator::CHILD) {
      // This means we are the daemon
      exit(system("htmd"));
    }
#endif
  }

  // This means we are the client to the daemon
  std::this_thread::sleep_for(std::chrono::microseconds(
      10 * 1000));  // Sleep for 10ms to let the daemon come alive
  shared_ptr<SocketHandler> socketHandler(new PipeSocketHandler());
  SocketEndpoint pipeEndpoint;
  pipeEndpoint.set_name(HtmServer::getPipeName());
  HtmClient htmClient(socketHandler, pipeEndpoint);
  htmClient.run();

  char buf[] = {
      0x1b, 0x5b, '$', '$', '$', 'q',
  };
#ifdef WIN32
  RawSocketUtils::writeAll(GetStdHandle(STD_OUTPUT_HANDLE), buf, sizeof(buf));
#else
  RawSocketUtils::writeAll(STDOUT_FILENO, buf, sizeof(buf));
#endif
  fflush(stdout);

#ifdef WIN32
  SetConsoleMode(hstdin, inputMode);
  SetConsoleMode(hstdout, outputMode);
#else
  tcsetattr(0, TCSANOW, &terminal_backup);
#endif

  return 0;
}
