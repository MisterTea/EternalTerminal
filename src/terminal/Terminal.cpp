#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "Headers.hpp"
#include "LogHandler.hpp"
#include "ParseConfigFile.hpp"
#include "PortForwardHandler.hpp"
#include "ServerConnection.hpp"
#include "SystemUtils.hpp"
#include "TcpSocketHandler.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"

#include "simpleini/SimpleIni.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#include <sys/socket.h>
#elif __NetBSD__  // do not need pty.h on NetBSD
#else
#include <pty.h>
#include <signal.h>
#endif

#include "ETerminal.pb.h"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

// This should be at least double the value of KEEP_ALIVE_DURATION in client to
// allow enough time.
const int KEEP_ALIVE_DURATION = 11;

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
  string first_idpass_chars = idpasskey.substr(0, 10);
  string std_file =
      string("/tmp/etserver_") + daemonType + "_" + first_idpass_chars;
  FILE *stdout_stream = freopen("/tmp/etclient_err", "w+", stdout);
  setvbuf(stdout_stream, NULL, _IOLBF, BUFSIZ);  // set to line buffering
  FILE *stderr_stream = freopen("/tmp/etclient_err", "w+", stderr);
  setvbuf(stderr_stream, NULL, _IOLBF, BUFSIZ);  // set to line buffering
}

void startUserTerminal(shared_ptr<SocketHandler> ipcSocketHandler,
                       string idpasskey) {
  UserTerminalHandler uth(ipcSocketHandler);
  uth.connectToRouter(idpasskey);
  cout << "IDPASSKEY:" << idpasskey << endl;
  if (::daemon(0, 0) == -1) {
    LOG(FATAL) << "Error creating daemon: " << strerror(errno);
  }
  setDaemonLogFile(idpasskey, "terminal");
  uth.run();
}

void startJumpHostClient(shared_ptr<SocketHandler> socketHandler,
                         string idpasskey) {
  cout << "IDPASSKEY:" << idpasskey << endl;
  auto idpasskey_splited = split(idpasskey, '/');
  string id = idpasskey_splited[0];
  string passkey = idpasskey_splited[1];

  string host = FLAGS_dsthost;
  int port = FLAGS_dstport;

  if (::daemon(0, 0) == -1) {
    LOG(FATAL) << "Error creating daemon: " << strerror(errno);
  }
  setDaemonLogFile(idpasskey, "jumphost");

  SocketEndpoint endpoint(ROUTER_FIFO_NAME);
  int routerFd = socketHandler->connect(endpoint);

  if (routerFd < 0) {
    if (errno == ECONNREFUSED) {
      cout << "Error:  The Eternal Terminal daemon is not running.  Please "
              "(re)start the et daemon on the server."
           << endl;
    } else {
      cout << "Error:  Connection error communicating with et deamon: "
           << strerror(errno) << "." << endl;
    }
    exit(1);
  }

  try {
    socketHandler->writePacket(
        routerFd, Packet(TerminalPacketType::IDPASSKEY, idpasskey));
  } catch (const std::runtime_error &re) {
    LOG(FATAL) << "Cannot send idpasskey to router: " << re.what();
  }

  InitialPayload payload;

  shared_ptr<SocketHandler> jumpclientSocket(new TcpSocketHandler());
  shared_ptr<ClientConnection> jumpclient =
      shared_ptr<ClientConnection>(new ClientConnection(
          jumpclientSocket, SocketEndpoint(host, port), id, passkey));

  int connectFailCount = 0;
  while (true) {
    try {
      jumpclient->connect();
      jumpclient->writeMessage(
          Packet(et::EtPacketType::INITIAL_PAYLOAD, protoToString(payload)));
    } catch (const runtime_error &err) {
      LOG(ERROR) << "Connecting to dst server failed: " << err.what();
      connectFailCount++;
      if (connectFailCount == 3) {
        LOG(INFO) << "Could not make initial connection to dst server";
        cout << "Could not make initial connection to " << host << ": "
             << err.what() << endl;
        exit(1);
      }
      continue;
    }
    break;
  }
  VLOG(1) << "JumpClient created with id: " << jumpclient->getId();

  bool run = true;
  bool is_reconnecting = false;
  time_t keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;

  while (run && !jumpclient->isShuttingDown()) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(routerFd, &rfd);
    int maxfd = routerFd;
    int jumpClientFd = jumpclient->getSocketFd();
    VLOG(4) << "Jump cliend fd: " << jumpClientFd;
    if (jumpClientFd > 0) {
      FD_SET(jumpClientFd, &rfd);
      maxfd = max(maxfd, jumpClientFd);
    }
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      // forward local router -> DST terminal.
      if (FD_ISSET(routerFd, &rfd)) {
        VLOG(4) << "Routerfd is selected";
        if (jumpClientFd < 0) {
          if (is_reconnecting) {
            // there is a reconnect thread running, joining...
            jumpclient->waitReconnect();
            is_reconnecting = false;
          } else {
            LOG(INFO) << "User comes back, reconnecting";
            is_reconnecting = true;
            jumpclient->closeSocket();
          }
          LOG(INFO) << "Reconnecting, sleep for 3s...";
          sleep(3);
          continue;
        } else {
          Packet p = socketHandler->readPacket(routerFd);
          jumpclient->writeMessage(p);
          VLOG(3) << "Sent message from router to dst terminal: " << p.length();
        }
        keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
      }
      // forward DST terminal -> local router
      if (jumpClientFd > 0 && FD_ISSET(jumpClientFd, &rfd)) {
        if (jumpclient->hasData()) {
          Packet receivedMessage;
          jumpclient->readMessage(&receivedMessage);
          socketHandler->writePacket(routerFd, receivedMessage);
          VLOG(3) << "Send message from dst terminal to router: "
                  << receivedMessage.length();
        }
        keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
      }
      // src disconnects, close jump -> dst
      if (jumpClientFd > 0 && keepaliveTime < time(NULL)) {
        LOG(INFO) << "Jumpclient idle, killing connection";
        jumpclient->Connection::closeSocket();
        is_reconnecting = false;
      }
    } catch (const runtime_error &re) {
      LOG(ERROR) << "Error: " << re.what();
      cout << "Connection closing because of error: " << re.what() << endl;
      run = false;
    }
  }
  LOG(ERROR) << "Jumpclient shutdown";
  close(routerFd);
}

int main(int argc, char **argv) {
  // Version string need to be set before GFLAGS parse arguments
  SetVersionString(string(ET_VERSION));

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::SetupLogHandler(&argc, &argv);

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
    // etserver with --jump cannot write to the default log file(root)
    LogHandler::SetupLogFile(&defaultConf,
                             "/tmp/etjump-" + username + "-" + id + ".log",
                             maxlogsize);
    // Reconfigure default logger to apply settings above
    el::Loggers::reconfigureLogger("default", defaultConf);
    // set thread name
    el::Helpers::setThreadName("jump-main");
    // Install log rotation callback
    el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

    startJumpHostClient(ipcSocketHandler, idpasskey);

    // Uninstall log rotation callback
    el::Helpers::uninstallPreRollOutCallback();
    return 0;
  }

  // etserver with --idpasskey cannot write to the default log file(root)
  LogHandler::SetupLogFile(&defaultConf,
                           "/tmp/etterminal-" + username + "-" + id + ".log",
                           maxlogsize);
  // Reconfigure default logger to apply settings above
  el::Loggers::reconfigureLogger("default", defaultConf);
  // set thread name
  el::Helpers::setThreadName("terminal-main");
  // Install log rotation callback
  el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

  startUserTerminal(ipcSocketHandler, idpasskey);

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
  return 0;
}
