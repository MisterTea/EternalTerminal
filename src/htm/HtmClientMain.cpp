#include "DaemonCreator.hpp"
#include "HtmClient.hpp"
#include "HtmServer.hpp"
#include "IpcPairClient.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "PipeSocketHandler.hpp"
#include "RawSocketUtils.hpp"
#include "SubprocessToString.hpp"

using namespace et;

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

int main(int argc, char** argv) {
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
  termios terminal_local;
  tcgetattr(0, &terminal_local);
  memcpy(&terminal_backup, &terminal_local, sizeof(struct termios));
  cfmakeraw(&terminal_local);
  tcsetattr(0, TCSANOW, &terminal_local);

  // Catch sigterm and send exit control code
  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));
  action.sa_handler = term;
  sigaction(SIGTERM, &action, NULL);

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
  el::Loggers::setVerboseLevel(3);
  // default max log file size is 20MB for etserver
  string maxlogsize = "20971520";
  LogHandler::setupLogFile(&defaultConf, "/tmp/htm.log", maxlogsize);
  // Redirect std streams to a file
  LogHandler::stderrToFile("/tmp/htm");

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

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
    int result = DaemonCreator::create(false, "");
    if (result == DaemonCreator::CHILD) {
      // This means we are the daemon
      exit(system("htmd"));
    }
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
  RawSocketUtils::writeAll(STDOUT_FILENO, buf, sizeof(buf));
  fflush(stdout);
  tcsetattr(0, TCSANOW, &terminal_backup);

  return 0;
}
