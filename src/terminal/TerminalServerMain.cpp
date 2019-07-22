#include "TerminalServer.hpp"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

DEFINE_int32(port, 0, "Port to listen on");
DEFINE_bool(daemon, false, "Daemonize the server");
DEFINE_string(cfgfile, "", "Location of the config file");
DEFINE_int32(v, 0, "verbose level");
DEFINE_bool(logtostdout, false, "log to stdout");

int main(int argc, char **argv) {
  // Version string need to be set before GFLAGS parse arguments
  SetVersionString(string(ET_VERSION));

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);

  // GFLAGS parse command line arguments
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_daemon) {
    if (DaemonCreator::create(true) == -1) {
      LOG(FATAL) << "Error creating daemon: " << strerror(errno);
    }
  }

  if (FLAGS_logtostdout) {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  } else {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
    // Redirect std streams to a file
    LogHandler::stderrToFile("/tmp/etserver");
  }

  // default max log file size is 20MB for etserver
  string maxlogsize = "20971520";

  if (FLAGS_cfgfile.length()) {
    // Load the config file
    CSimpleIniA ini(true, true, true);
    SI_Error rc = ini.LoadFile(FLAGS_cfgfile.c_str());
    if (rc == 0) {
      if (FLAGS_port == 0) {
        const char *portString = ini.GetValue("Networking", "Port", NULL);
        if (portString) {
          FLAGS_port = stoi(portString);
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
      LOG(FATAL) << "Invalid config file: " << FLAGS_cfgfile;
    }
  }

  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);

  if (FLAGS_port == 0) {
    FLAGS_port = 2022;
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

  TerminalServer terminalServer(tcpSocketHandler, SocketEndpoint(FLAGS_port),
                                pipeSocketHandler,
                                SocketEndpoint(ROUTER_FIFO_NAME));
  terminalServer.run();

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
}
