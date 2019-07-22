#include "DaemonCreator.hpp"
#include "LogHandler.hpp"
#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PsuedoUserTerminal.hpp"
#include "TcpSocketHandler.hpp"
#include "UserJumphostHandler.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"
#include "simpleini/SimpleIni.h"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

DEFINE_string(idpasskey, "",
              "If set, uses IPC to send a client id/key to the server daemon");
DEFINE_string(idpasskeyfile, "",
              "If set, uses IPC to send a client id/key to the server daemon "
              "from a file");
DEFINE_bool(jump, false,
            "If set, forward all packets between client and dst terminal");
DEFINE_string(dsthost, "", "Must be set if jump is set to true");
DEFINE_int32(dstport, 2022, "Must be set if jump is set to true");
DEFINE_int32(v, 0, "verbose level");
DEFINE_bool(logtostdout, false, "log to stdout");
DEFINE_string(cfgfile, "", "Location of the config file");
DEFINE_bool(noratelimit, false, "Disable rate limit");

string getIdpasskey() {
  string idpasskey = FLAGS_idpasskey;
  if (FLAGS_idpasskeyfile.length() > 0) {
    // Check for passkey file
    std::ifstream t(FLAGS_idpasskeyfile.c_str());
    std::stringstream buffer;
    buffer << t.rdbuf();
    idpasskey = buffer.str();
    // Trim whitespace
    idpasskey.erase(idpasskey.find_last_not_of(" \n\r\t") + 1);
    // Delete the file with the passkey
    remove(FLAGS_idpasskeyfile.c_str());
  }
  return idpasskey;
}

void setDaemonLogFile(string idpasskey, string daemonType) {
  if (!FLAGS_logtostdout) {
    string first_idpass_chars = idpasskey.substr(0, 10);
    string logFile =
        string("/tmp/etterminal_") + daemonType + "_" + first_idpass_chars;
    // Redirect std streams to a file
    LogHandler::stderrToFile(logFile);
  }
}

int main(int argc, char **argv) {
  // Version string need to be set before GFLAGS parse arguments
  SetVersionString(string(ET_VERSION));

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);

  // GFLAGS parse command line arguments
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_logtostdout) {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  } else {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
  }

  // default max log file size is 20MB for etserver
  string maxlogsize = "20971520";

  if (FLAGS_cfgfile.length()) {
    // Load the config file
    CSimpleIniA ini(true, true, true);
    SI_Error rc = ini.LoadFile(FLAGS_cfgfile.c_str());
    if (rc == 0) {
      // read verbose level
      const char *vlevel = ini.GetValue("Debug", "verbose", NULL);
      if (vlevel) {
        el::Loggers::setVerboseLevel(atoi(vlevel));
      }
      // read silent setting
      const char *silent = ini.GetValue("Debug", "silent", NULL);
      if (silent && atoi(silent) != 0) {
        defaultConf.setGlobally(el::ConfigurationType::Enabled, "false");
      }
      // read log file size limit
      const char *logsize = ini.GetValue("Debug", "logsize", NULL);
      if (logsize && atoi(logsize) != 0) {
        // make sure maxlogsize is a string of int value
        maxlogsize = string(logsize);
      }

    } else {
      LOG(FATAL) << "Invalid config file: " << FLAGS_cfgfile;
    }
  }

  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);

  shared_ptr<SocketHandler> ipcSocketHandler(new PipeSocketHandler());
  shared_ptr<PsuedoUserTerminal> term(new PsuedoUserTerminal());

  if (FLAGS_idpasskey.length() == 0 && FLAGS_idpasskeyfile.length() == 0) {
    // Try to read from stdin
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    fd_set readfds;
    FD_ZERO(&readfds);

    FD_SET(STDIN_FILENO, &readfds);

    int res = select(1, &readfds, NULL, NULL, &timeout);
    if (res < 0) {
      FATAL_FAIL(res);
    }
    if (res == 0) {
      cout << "Call etterminal with --idpasskey or --idpasskeyfile, or feed "
              "this information on stdin\n";
      exit(1);
    }

    string stdinData;
    if (!getline(cin, stdinData)) {
      cout << "Call etterminal with --idpasskey or --idpasskeyfile, or feed "
              "this information on stdin\n";
      exit(1);
    }
    auto tokens = split(stdinData, '_');
    FLAGS_idpasskey = tokens[0];
    FATAL_FAIL(setenv("TERM", tokens[1].c_str(), 1));
  }

  string idpasskey = getIdpasskey();
  string id = split(idpasskey, '/')[0];
  string username = string(ssh_get_local_username());
  if (FLAGS_jump) {
    setDaemonLogFile(idpasskey, "jumphost");

    // etserver with --jump cannot write to the default log file(root)
    LogHandler::setupLogFile(&defaultConf,
                             "/tmp/etjump-" + username + "-" + id + ".log",
                             maxlogsize);
    // Reconfigure default logger to apply settings above
    el::Loggers::reconfigureLogger("default", defaultConf);
    // set thread name
    el::Helpers::setThreadName("jump-main");
    // Install log rotation callback
    el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

    cout << "IDPASSKEY:" << idpasskey << endl;
    if (DaemonCreator::create(true) == -1) {
      LOG(FATAL) << "Error creating daemon: " << strerror(errno);
    }
    shared_ptr<SocketHandler> jumpClientSocketHandler(new TcpSocketHandler());
    UserJumphostHandler ujh(jumpClientSocketHandler, idpasskey,
                            SocketEndpoint(FLAGS_dsthost, FLAGS_dstport),
                            ipcSocketHandler, SocketEndpoint(ROUTER_FIFO_NAME));
    ujh.run();

    // Uninstall log rotation callback
    el::Helpers::uninstallPreRollOutCallback();
    return 0;
  }

  setDaemonLogFile(idpasskey, "terminal");

  // etserver with --idpasskey cannot write to the default log file(root)
  LogHandler::setupLogFile(&defaultConf,
                           "/tmp/etterminal-" + username + "-" + id + ".log",
                           maxlogsize);
  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);
  // set thread name
  el::Helpers::setThreadName("terminal-main");
  // Install log rotation callback
  el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

  UserTerminalHandler uth(ipcSocketHandler, term, FLAGS_noratelimit,
                          SocketEndpoint(ROUTER_FIFO_NAME), idpasskey);
  cout << "IDPASSKEY:" << idpasskey << endl;
  if (DaemonCreator::create(true) == -1) {
    LOG(FATAL) << "Error creating daemon: " << strerror(errno);
  }
  uth.run();

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
  return 0;
}
