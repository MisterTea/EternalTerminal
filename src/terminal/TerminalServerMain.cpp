#include "TerminalServer.hpp"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

int main(int argc, char **argv) {
  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);

  // Parse command line arguments
  cxxopts::Options options("etserver",
                           "Remote shell for the busy and impatient");
  options.allow_unrecognised_options();

  options.add_options()             //
      ("help", "Print help")        //
      ("version", "Print version")  //
      ("port", "Port to listen on",
       cxxopts::value<int>()->default_value("0"))  //
      ("daemon", "Daemonize the server")           //
      ("cfgfile", "Location of the config file",
       cxxopts::value<std::string>()->default_value(""))  //
      ("logtostdout", "log to stdout")                    //
      ("pidfile", "Location of the pid file",
        cxxopts::value<std::string>()->default_value("/var/run/etserver.pid"))  //
      ("v,verbose", "Enable verbose logging",
       cxxopts::value<int>()->default_value("0"))  //
      ;

  auto result = options.parse(argc, argv);
  if (result.count("help")) {
    cout << options.help({}) << endl;
    exit(0);
  }
  if (result.count("version")) {
    cout << "et version " << ET_VERSION << endl;
    exit(0);
  }

  if (result.count("daemon")) {
    if (DaemonCreator::create(true, result["pidfile"].as<string>()) == -1) {
      LOG(FATAL) << "Error creating daemon: " << strerror(errno);
    }
  }

  if (result.count("logtostdout")) {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  } else {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
    // Redirect std streams to a file
    LogHandler::stderrToFile("/tmp/etserver");
  }

  // default max log file size is 20MB for etserver
  string maxlogsize = "20971520";

  int port = 0;
  if (result.count("cfgfile")) {
    // Load the config file
    CSimpleIniA ini(true, true, true);
    string cfgfilename = result["cfgfile"].as<string>();
    SI_Error rc = ini.LoadFile(cfgfilename.c_str());
    if (rc == 0) {
      if (!result.count("port")) {
        const char *portString = ini.GetValue("Networking", "Port", NULL);
        if (portString) {
          port = stoi(portString);
        }
      }
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
      LOG(FATAL) << "Invalid config file: " << cfgfilename;
    }
  }

  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);

  if (port == 0) {
    port = 2022;
  }

  // Set log file for etserver process here.
  LogHandler::setupLogFile(&defaultConf, "/tmp/etserver-%datetime.log",
                           maxlogsize);
  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);
  // set thread name
  el::Helpers::setThreadName("etserver-main");
  // Install log rotation callback
  el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);
  std::shared_ptr<SocketHandler> tcpSocketHandler(new TcpSocketHandler());
  std::shared_ptr<PipeSocketHandler> pipeSocketHandler(new PipeSocketHandler());

  LOG(INFO) << "In child, about to start server.";

  TerminalServer terminalServer(tcpSocketHandler, SocketEndpoint(port),
                                pipeSocketHandler,
                                SocketEndpoint(ROUTER_FIFO_NAME));
  terminalServer.run();

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
}
