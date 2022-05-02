#include <cxxopts.hpp>

#include "ServerFifoPath.hpp"
#include "SimpleIni.h"
#include "TelemetryService.hpp"
#include "TerminalServer.hpp"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

int main(int argc, char **argv) {
  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  LogHandler::setupStdoutLogger();

  et::HandleTerminate();

  // Override easylogging handler for sigint
  ::signal(SIGINT, et::InterruptSignalHandler);

  cxxopts::Options options("etserver",
                           "Remote shell for the busy and impatient");
  try {
    // Parse command line arguments
    options.allow_unrecognised_options();

    options.add_options()             //
        ("h,help", "Print help")      //
        ("version", "Print version")  //
        ("port", "Port to listen on",
         cxxopts::value<int>()->default_value("0"))  //
        ("bindip", "IP to listen on",
         cxxopts::value<string>()->default_value(""))  //
        ("daemon", "Daemonize the server")             //
        ("cfgfile", "Location of the config file",
         cxxopts::value<std::string>()->default_value(""))  //
        ("logtostdout", "log to stdout")                    //
        ("pidfile", "Location of the pid file",
         cxxopts::value<std::string>()->default_value(
             "/var/run/etserver.pid"))  //
        ("v,verbose", "Enable verbose logging",
         cxxopts::value<int>()->default_value("0"), "LEVEL")  //
        ("serverfifo",
         "If set, listens on the matching fifo name",       //
         cxxopts::value<std::string>()->default_value(""))  //
        ("telemetry",
         "Allow et to anonymously send errors to guide future improvements",
         cxxopts::value<bool>())  //
        ;

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }
    if (result.count("version")) {
      CLOG(INFO, "stdout") << "et version " << ET_VERSION << endl;
      exit(0);
    }

    if (result.count("daemon")) {
      if (DaemonCreator::create(true, result["pidfile"].as<string>()) == -1) {
        STFATAL << "Error creating daemon: " << strerror(GetErrno());
      }
    }

    if (result.count("logtostdout")) {
      defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
    } else {
      defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
      // Redirect std streams to a file
      LogHandler::stderrToFile(GetTempDirectory() + "etserver");
    }

    ServerFifoPath serverFifo;

    // default max log file size is 20MB for etserver
    string maxlogsize = "20971520";

    int port = 0;
    string bindIp = "";
    bool telemetry = true;
    if (result.count("cfgfile")) {
      // Load the config file
      CSimpleIniA ini(true, false, false);
      string cfgfilename = result["cfgfile"].as<string>();
      SI_Error rc = ini.LoadFile(cfgfilename.c_str());
      if (rc == 0) {
        if (!result.count("port")) {
          const char *portString = ini.GetValue("Networking", "port", NULL);
          if (portString) {
            port = stoi(portString);
          }
        }

        if (!result.count("bindip")) {
          const char *bindIpPtr = ini.GetValue("Networking", "bind_ip", NULL);
          if (bindIpPtr) {
            bindIp = string(bindIpPtr);
          }
        }

        telemetry = bool(stoi(ini.GetValue("Debug", "Telemetry", "1")));
        // read verbose level (prioritize command line option over cfgfile)
        const char *vlevel = ini.GetValue("Debug", "verbose", NULL);
        if (result.count("verbose")) {
          el::Loggers::setVerboseLevel(result["verbose"].as<int>());
        } else if (vlevel) {
          el::Loggers::setVerboseLevel(atoi(vlevel));
        }

        const char *fifoName = ini.GetValue("Debug", "serverfifo", NULL);
        if (fifoName) {
          const string fifoNameStr(fifoName);
          if (!fifoNameStr.empty()) {
            serverFifo.setPathOverride(fifoNameStr);
          }
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
        STFATAL << "Invalid config file: " << cfgfilename;
      }
    }

    if (result.count("serverfifo") &&
        !result["serverfifo"].as<string>().empty()) {
      serverFifo.setPathOverride(result["serverfifo"].as<string>());
    }

    if (result.count("port")) {
      port = result["port"].as<int>();
    }

    if (result.count("bindip")) {
      bindIp = result["bindip"].as<string>();
    }

    if (result.count("telemetry")) {
      telemetry = result["telemetry"].as<bool>();
    }

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    srand(1);

    if (port == 0) {
      port = 2022;
    }

    // Set log file for etserver process here.
    LogHandler::setupLogFile(&defaultConf,
                             GetTempDirectory() + "etserver-%datetime.log",
                             maxlogsize);
    // Reconfigure default logger to apply settings above
    el::Loggers::reconfigureLogger("default", defaultConf);
    // set thread name
    el::Helpers::setThreadName("etserver-main");
    // Install log rotation callback
    el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

    TelemetryService::create(
        telemetry, GetTempDirectory() + "/.sentry-native-etserver", "Server");

    serverFifo.createDirectoriesIfRequired();

    std::shared_ptr<SocketHandler> tcpSocketHandler(new TcpSocketHandler());
    std::shared_ptr<PipeSocketHandler> pipeSocketHandler(
        new PipeSocketHandler());

    LOG(INFO) << "In child, about to start server.";

    SocketEndpoint serverEndpoint;
    serverEndpoint.set_port(port);
    if (bindIp.length()) {
      serverEndpoint.set_name(bindIp);
    }
    SocketEndpoint routerFifo;
    routerFifo.set_name(serverFifo.getPathForCreation());
    TerminalServer terminalServer(tcpSocketHandler, serverEndpoint,
                                  pipeSocketHandler, routerFifo);
    terminalServer.run();

  } catch (cxxopts::OptionException &oe) {
    CLOG(INFO, "stdout") << "Exception: " << oe.what() << "\n" << endl;
    CLOG(INFO, "stdout") << options.help({}) << endl;
    exit(1);
  }

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
}
