#include <cxxopts.hpp>

#include "DaemonCreator.hpp"
#include "LogHandler.hpp"
#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PseudoUserTerminal.hpp"
#include "ServerFifoPath.hpp"
#include "TcpSocketHandler.hpp"
#include "TerminalStdinParsing.hpp"
#include "UserJumphostHandler.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"
#include "WinsockContext.hpp"
#ifdef WIN32
#include <windows.h>
#endif

using namespace et;

namespace {
inline bool HasNonEmptyOption(const cxxopts::ParseResult& result,
                              const string& name) {
  if (result.count(name) == 0) {
    return false;
  }
  // cxxopts counts default_value("") as present; treat empty as absent so
  // stdin fallback still works.
  try {
    return !result[name].as<string>().empty();
  } catch (...) {
    return true;
  }
}

inline void SetTermEnv(const char* value) {
#ifdef WIN32
  SetEnvironmentVariableA("TERM", value);
  // Also update the CRT environment for getenv("TERM") consumers.
  string entry = string("TERM=") + value;
  _putenv(entry.c_str());
#else
  FATAL_FAIL(setenv("TERM", value, 1));
#endif
}
}  // namespace

int main(int argc, char** argv) {
#ifdef WIN32
  WinsockContext winsockContext;
#endif
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
        ("l,logdir", "Base directory for log files.",
         cxxopts::value<std::string>()->default_value(GetTempDirectory()))  //
        ("logtostdout", "Write log to stdout")                              //
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

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    srand(1);

    ServerFifoPath serverFifo;
    if (!result["serverfifo"].as<string>().empty()) {
      serverFifo.setPathOverride(result["serverfifo"].as<string>());
    }

    shared_ptr<SocketHandler> ipcSocketHandler(new PipeSocketHandler());
    shared_ptr<PseudoUserTerminal> term(new PseudoUserTerminal());

    string idpasskey;
    if (!HasNonEmptyOption(result, "idpasskey") &&
        !HasNonEmptyOption(result, "idpasskeyfile")) {
      // Try to read from stdin
#ifdef WIN32
      HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
      DWORD waitResult = WaitForSingleObject(stdinHandle, 1000);
      if (waitResult != WAIT_OBJECT_0) {
        CLOG(INFO, "stdout")
            << "Call etterminal with --idpasskey or --idpasskeyfile, or feed "
               "this information on stdin\n";
        exit(1);
      }
#else
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
#endif

      string stdinData;
      if (!getline(cin, stdinData)) {
        CLOG(INFO, "stdout")
            << "Call etterminal with --idpasskey or --idpasskeyfile, or feed "
               "this information on stdin\n";
        exit(1);
      }
      TerminalStdinLine stdinLine;
      if (parseTerminalStdinLine(stdinData, &stdinLine)) {
        idpasskey = stdinLine.idpasskey;
        if (idpasskey.substr(0, 3) == std::string("XXX")) {
          // New client connecting to new server, throw away passkey and
          // regenerate
          string passkey = genRandomAlphaNum(32);
          string id = genRandomAlphaNum(16);
          idpasskey = id + string("/") + passkey;
        }

        SetTermEnv(stdinLine.term.c_str());
      } else {
        STFATAL << "Invalid stdin line: expected <id>/<passkey>_<TERM>";
      }
    } else {
      idpasskey = result["idpasskey"].as<string>();
      if (HasNonEmptyOption(result, "idpasskeyfile")) {
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
      LogHandler::setupLogFiles(&defaultConf, result["logdir"].as<string>(),
                                ("etjump-" + username + "-" + id),
                                result.count("logtostdout"), false);
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
    LogHandler::setupLogFiles(&defaultConf, result["logdir"].as<string>(),
                              ("etterminal-" + username + "-" + id),
                              result.count("logtostdout"), false);

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

  } catch (cxxopts::exceptions::exception& oe) {
    CLOG(INFO, "stdout") << "Exception: " << oe.what() << "\n" << endl;
    CLOG(INFO, "stdout") << options.help({}) << endl;
    exit(1);
  }

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
  return 0;
}
