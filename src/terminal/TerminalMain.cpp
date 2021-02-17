#include "DaemonCreator.hpp"
#include "LogHandler.hpp"
#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PsuedoUserTerminal.hpp"
#include "TcpSocketHandler.hpp"
#include "UserJumphostHandler.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"

using namespace et;

void setDaemonLogFile(string idpasskey, string daemonType) {
  string first_idpass_chars = idpasskey.substr(0, 10);
  string logFile = string(GetTempDirectory() + "etterminal_") + daemonType +
                   "_" + first_idpass_chars;
}

int main(int argc, char** argv) {
  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  LogHandler::setupStdoutLogger();

  // Parse command line arguments
  cxxopts::Options options("et", "Remote shell for the busy and impatient");

  try {
    options.positional_help("[user@]hostname[:port]").show_positional_help();
    options.allow_unrecognised_options();

    options.add_options()         //
        ("h,help", "Print help")  //
        ("idpasskey",
         "If set, uses IPC to send a client id/key to the server daemon",
         cxxopts::value<std::string>()->default_value(""))  //
        ("idpasskeyfile",
         "If set, uses IPC to send a client id/key to the server daemon from a "
         "file",
         cxxopts::value<std::string>()->default_value(""))  //
        ("jump",
         "If set, forward all packets between client and dst terminal")  //
        ("dsthost", "Must be set if jump is set to true",
         cxxopts::value<std::string>()->default_value(""))  //
        ("dstport", "Must be set if jump is set to true",
         cxxopts::value<int>()->default_value("2022"))  //
        ("v,verbose", "Enable verbose logging",
         cxxopts::value<int>()->default_value("0"))  //
        ("logtostdout", "Write log to stdout")       //
        ("serverfifo",
         "If set, connects to the etserver instance listening on the matching "
         "fifo name",                                                     //
         cxxopts::value<std::string>()->default_value(ROUTER_FIFO_NAME))  //
        ;

    options.parse_positional({"host", "positional"});

    auto result = options.parse(argc, argv);
    if (result.count("help")) {
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }

    if (result.count("logtostdout")) {
      defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
    } else {
      defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
    }

    // default max log file size is 20MB for etserver
    string maxlogsize = "20971520";

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    srand(1);

    shared_ptr<SocketHandler> ipcSocketHandler(new PipeSocketHandler());
    shared_ptr<PsuedoUserTerminal> term(new PsuedoUserTerminal());

    string idpasskey;
    if (result.count("idpasskey") == 0 && result.count("idpasskeyfile") == 0) {
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
        CLOG(INFO, "stdout")
            << "Call etterminal with --idpasskey or --idpasskeyfile, or feed "
               "this information on stdin\n";
        exit(1);
      }

      string stdinData;
      if (!getline(cin, stdinData)) {
        CLOG(INFO, "stdout")
            << "Call etterminal with --idpasskey or --idpasskeyfile, or feed "
               "this information on stdin\n";
        exit(1);
      }
      auto tokens = split(stdinData, '_');
      if (tokens.size() == 2) {
        idpasskey = tokens[0];
        if (idpasskey.substr(0, 3) == std::string("XXX")) {
          // New client connecting to new server, throw away passkey and
          // regenerate
          string passkey = genRandomAlphaNum(32);
          string id = genRandomAlphaNum(16);
          idpasskey = id + string("/") + passkey;
        }

        FATAL_FAIL(setenv("TERM", tokens[1].c_str(), 1));
      } else {
        STFATAL << "Invalid number of tokens: " << tokens.size();
      }
    } else {
      string idpasskey = result["idpasskey"].as<string>();
      if (result.count("idpasskeyfile")) {
        // Check for passkey file
        std::ifstream t(result["idpasskeyfile"].as<string>().c_str());
        std::stringstream buffer;
        buffer << t.rdbuf();
        idpasskey = buffer.str();
        // Trim whitespace
        idpasskey.erase(idpasskey.find_last_not_of(" \n\r\t") + 1);
      }
    }

    string id = split(idpasskey, '/')[0];
    string username = string(ssh_get_local_username());
    if (result.count("jump")) {
      setDaemonLogFile(idpasskey, "jumphost");

      // etserver with --jump cannot write to the default log file(root)
      LogHandler::setupLogFile(
          &defaultConf,
          GetTempDirectory() + "etjump-" + username + "-" + id + ".log",
          maxlogsize);
      // Reconfigure default logger to apply settings above
      el::Loggers::reconfigureLogger("default", defaultConf);
      // set thread name
      el::Helpers::setThreadName("jump-main");
      // Install log rotation callback
      el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

      CLOG(INFO, "stdout") << "IDPASSKEY:" << idpasskey << endl;
      if (DaemonCreator::createSessionLeader() == -1) {
        STFATAL << "Error creating daemon: " << strerror(errno);
      }
      SocketEndpoint routerFifoEndpoint;
      routerFifoEndpoint.set_name(result["serverfifo"].as<string>());
      SocketEndpoint destinationEndpoint;
      destinationEndpoint.set_name(result["dsthost"].as<string>());
      destinationEndpoint.set_port(result["dstport"].as<int>());
      shared_ptr<SocketHandler> jumpClientSocketHandler(new TcpSocketHandler());
      UserJumphostHandler ujh(jumpClientSocketHandler, idpasskey,
                              destinationEndpoint, ipcSocketHandler,
                              routerFifoEndpoint);
      ujh.run();

      // Uninstall log rotation callback
      el::Helpers::uninstallPreRollOutCallback();
      return 0;
    }

    setDaemonLogFile(idpasskey, "terminal");

    // etserver with --idpasskey cannot write to the default log file(root)
    LogHandler::setupLogFile(
        &defaultConf,
        GetTempDirectory() + "etterminal-" + username + "-" + id + ".log",
        maxlogsize);
    // Reconfigure default logger to apply settings above
    el::Loggers::reconfigureLogger("default", defaultConf);
    // set thread name
    el::Helpers::setThreadName("terminal-main");
    // Install log rotation callback
    el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

    SocketEndpoint routerEndpoint;
    routerEndpoint.set_name(result["serverfifo"].as<string>());
    UserTerminalHandler uth(ipcSocketHandler, term, true, routerEndpoint,
                            idpasskey);
    CLOG(INFO, "stdout") << "IDPASSKEY:" << idpasskey << endl;
    if (DaemonCreator::createSessionLeader() == -1) {
      STFATAL << "Error creating daemon: " << strerror(errno);
    }
    uth.run();

  } catch (cxxopts::OptionException& oe) {
    CLOG(INFO, "stdout") << "Exception: " << oe.what() << "\n" << endl;
    CLOG(INFO, "stdout") << options.help({}) << endl;
    exit(1);
  }

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
  return 0;
}
