#include "DaemonCreator.hpp"
#include "HtmClient.hpp"
#include "HtmServer.hpp"
#include "IpcPairClient.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "RawSocketUtils.hpp"

DEFINE_bool(x, false, "flag to kill all old sessions belonging to the user");

termios terminal_backup;

void term(int signum) {
  char buf[] = {
      0x1b, 0x5b, '$', '$', '$', 'q',
  };
  et::RawSocketUtils::writeAll(STDOUT_FILENO, buf, sizeof(buf));
  fflush(stdout);
  tcsetattr(0, TCSANOW, &terminal_backup);
  exit(1);
}

int main(int argc, char** argv) {
  // Version string need to be set before GFLAGS parse arguments
  SetVersionString(string(ET_VERSION));
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);

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
  el::Configurations defaultConf =
      et::LogHandler::SetupLogHandler(&argc, &argv);
  defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
  el::Loggers::setVerboseLevel(3);
  // default max log file size is 20MB for etserver
  string maxlogsize = "20971520";
  et::LogHandler::SetupLogFile(&defaultConf, "/tmp/htm.log", maxlogsize);

  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);

  uid_t myuid = getuid();
  if (FLAGS_x) {
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
    int result = et::DaemonCreator::create();
    if (result == et::DaemonCreator::CHILD) {
      // This means we are the daemon
      exit(system("htmd"));
    }
  }

  // This means we are the client to the daemon
  usleep(10 * 1000);  // Sleep for 10ms to let the daemon come alive
  et::HtmClient htmClient;
  try {
    htmClient.run();
  } catch (const std::runtime_error& ex) {
    LOG(ERROR) << ex.what();
  }

  char buf[] = {
      0x1b, 0x5b, '$', '$', '$', 'q',
  };
  et::RawSocketUtils::writeAll(STDOUT_FILENO, buf, sizeof(buf));
  fflush(stdout);
  tcsetattr(0, TCSANOW, &terminal_backup);

  return 0;
}
