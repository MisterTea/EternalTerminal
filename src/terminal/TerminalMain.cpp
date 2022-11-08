#include <cxxopts.hpp>

#include "DaemonCreator.hpp"
#include "LogHandler.hpp"
#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PsuedoUserTerminal.hpp"
#include "ServerFifoPath.hpp"
#include "TcpSocketHandler.hpp"
#include "UserJumphostHandler.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"

using namespace et;

int main(int argc, char** argv) {
  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  LogHandler::setupStdoutLogger();

  et::HandleTerminate();

  // Override easylogging handler for sigint
  ::signal(SIGINT, et::InterruptSignalHandler);

  // Parse command line arguments
  cxxopts::Options options("etterminal", "User terminal for Eternal Terminal.");

  try {
    options.allow_unrecognised_options();

    options.add_options()         //
        ("h,help", "Print help")  //
        ("idpasskey",
         "If set, uses IPC to send a client id/key to the server daemon. "
         "Alternatively, pass in via stdin.",
         cxxopts::value<std::string>()->default_value(""))  //
        ("idpasskeyfile",
         "If set, uses IPC to send a client id/key to the server daemon from a "
         "file. Alternatively, pass in via stdin.",
         cxxopts::value<std::string>()->default_value(""))  //
        ("jump",
         "If set, forward all packets between client and dst terminal")  //
        ("dsthost", "Must be set if jump is set to true",
         cxxopts::value<std::string>()->default_value(""))  //
        ("dstport", "Must be set if jump is set to true",
         cxxopts::value<int>()->default_value("2022"))  //
        // Not used by etterminal but easylogging uses this flag under the hood
        ("v,verbose", "Enable verbose logging",
         cxxopts::value<int>()->default_value("0"))  //
        ("logtostdout", "Write log to stdout")       //
        ("serverfifo",
         "If set, connects to the etserver instance listening on the matching "
         "fifo name",                                       //
         cxxopts::value<std::string>()->default_value(""))  //
        ;

    auto result = options.parse(argc, argv);
    if (result.count("help")) {
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }

    el::Loggers::setVerboseLevel(result["verbose"].as<int>());

    if (result.count("logtostdout")) {
      defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
    } else {
      defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
    }

    // default max log file size is 20MB for etserver
    string maxlogsize = "20971520";

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    srand(1);

    ServerFifoPath serverFifo;
    if (!result["serverfifo"].as<string>().empty()) {
      serverFifo.setPathOverride(result["serverfifo"].as<string>());
    }

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

      int selectResult = 0;
      do {
        // Repeatedly calls when interrupted, up to the timeout.
        selectResult = select(1, &readfds, NULL, NULL, &timeout);
      } while (selectResult < 0 && errno == EINTR);

      FATAL_FAIL(selectResult);
      if (selectResult == 0) {
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
        STFATAL << "Error creating daemon: " << strerror(GetErrno());
      }
      SocketEndpoint destinationEndpoint;
      destinationEndpoint.set_name(result["dsthost"].as<string>());
      destinationEndpoint.set_port(result["dstport"].as<int>());
      shared_ptr<SocketHandler> jumpClientSocketHandler(new TcpSocketHandler());
      UserJumphostHandler ujh(jumpClientSocketHandler, idpasskey,
                              destinationEndpoint, ipcSocketHandler,
                              serverFifo.getEndpointForConnect());
      ujh.run();

      // Uninstall log rotation callback
      el::Helpers::uninstallPreRollOutCallback();
      return 0;
    }

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

    UserTerminalHandler uth(ipcSocketHandler, term, true,
                            serverFifo.getEndpointForConnect(), idpasskey);
    CLOG(INFO, "stdout") << "IDPASSKEY:" << idpasskey << endl;
    if (DaemonCreator::createSessionLeader() == -1) {
      STFATAL << "Error creating daemon: " << strerror(GetErrno());
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
