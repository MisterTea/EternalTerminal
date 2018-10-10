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

#define BUF_SIZE (16 * 1024)

DEFINE_int32(port, 0, "Port to listen on");
DEFINE_bool(daemon, false, "Daemonize the server");
DEFINE_string(cfgfile, "", "Location of the config file");
DEFINE_int32(v, 0, "verbose level");
DEFINE_bool(logtostdout, false, "log to stdout");

shared_ptr<ServerConnection> globalServer;
shared_ptr<UserTerminalRouter> terminalRouter;
vector<shared_ptr<thread>> terminalThreads;
mutex terminalThreadMutex;
bool halt = false;

void runJumpHost(shared_ptr<ServerClientConnection> serverClientState) {
  // set thread name
  el::Helpers::setThreadName(serverClientState->getId());
  bool run = true;

  bool b[BUF_SIZE];
  int terminalFd = terminalRouter->getFd(serverClientState->getId());
  shared_ptr<SocketHandler> terminalSocketHandler =
      terminalRouter->getSocketHandler();

  while (!halt && run) {
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(terminalFd, &rfd);
    int maxfd = terminalFd;
    int serverClientFd = serverClientState->getSocketFd();
    if (serverClientFd > 0) {
      FD_SET(serverClientFd, &rfd);
      maxfd = max(maxfd, serverClientFd);
    }
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      if (FD_ISSET(terminalFd, &rfd)) {
        memset(b, 0, BUF_SIZE);
        try {
          string message = terminalSocketHandler->readMessage(terminalFd);
          serverClientState->writeMessage(message);
        } catch (const std::runtime_error &ex) {
          LOG(INFO) << "Terminal session ended" << ex.what();
          run = false;
          break;
        }
      }

      VLOG(4) << "Jumphost serverclientFd: " << serverClientFd;
      if (serverClientFd > 0 && FD_ISSET(serverClientFd, &rfd)) {
        VLOG(4) << "Jumphost is selected";
        if (serverClientState->hasData()) {
          VLOG(4) << "Jumphost serverClientState has data";
          string message;
          if (!serverClientState->readMessage(&message)) {
            break;
          }
          try {
            terminalSocketHandler->writeMessage(terminalFd, message);
            VLOG(4) << "Jumphost wrote to router " << terminalFd;
          } catch (const std::runtime_error &ex) {
            LOG(INFO) << "Unix socket died between global daemon and terminal "
                         "router: "
                      << ex.what();
            run = false;
            break;
          }
        }
      }
    } catch (const runtime_error &re) {
      LOG(ERROR) << "Jumphost Error: " << re.what();
      cerr << "ERROR: " << re.what();
      serverClientState->closeSocket();
    }
  }
  {
    string id = serverClientState->getId();
    serverClientState.reset();
    globalServer->removeClient(id);
  }
}

void runTerminal(shared_ptr<ServerClientConnection> serverClientState) {
  // Set thread name
  el::Helpers::setThreadName(serverClientState->getId());
  // Whether the TE should keep running.
  bool run = true;

  // TE sends/receives data to/from the shell one char at a time.
  char b[BUF_SIZE];

  shared_ptr<SocketHandler> serverSocketHandler =
      globalServer->getSocketHandler();
  PortForwardHandler portForwardHandler(serverSocketHandler);

  int terminalFd = terminalRouter->getFd(serverClientState->getId());
  shared_ptr<SocketHandler> terminalSocketHandler =
      terminalRouter->getSocketHandler();

  while (!halt && run) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(terminalFd, &rfd);
    int maxfd = terminalFd;
    int serverClientFd = serverClientState->getSocketFd();
    if (serverClientFd > 0) {
      FD_SET(serverClientFd, &rfd);
      maxfd = max(maxfd, serverClientFd);
    }
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      // Check for data to receive; the received
      // data includes also the data previously sent
      // on the same master descriptor (line 90).
      if (FD_ISSET(terminalFd, &rfd)) {
        // Read from terminal and write to client
        memset(b, 0, BUF_SIZE);
        int rc = read(terminalFd, b, BUF_SIZE);
        if (rc > 0) {
          VLOG(2) << "Sending bytes from terminal: " << rc << " "
                  << serverClientState->getWriter()->getSequenceNumber();
          char c = et::PacketType::TERMINAL_BUFFER;
          serverClientState->writeMessage(string(1, c));
          string s(b, rc);
          et::TerminalBuffer tb;
          tb.set_buffer(s);
          serverClientState->writeProto(tb);
        } else {
          LOG(INFO) << "Terminal session ended";
          run = false;
          globalServer->removeClient(serverClientState->getId());
          break;
        }
      }

      vector<PortForwardDestinationRequest> requests;
      vector<PortForwardData> dataToSend;
      portForwardHandler.update(&requests, &dataToSend);
      for (auto &pfr : requests) {
        char c = et::PacketType::PORT_FORWARD_DESTINATION_REQUEST;
        string headerString(1, c);
        serverClientState->writeMessage(headerString);
        serverClientState->writeProto(pfr);
      }
      for (auto &pwd : dataToSend) {
        char c = PacketType::PORT_FORWARD_DATA;
        string headerString(1, c);
        serverClientState->writeMessage(headerString);
        serverClientState->writeProto(pwd);
      }

      VLOG(3) << "ServerClientFd: " << serverClientFd;
      if (serverClientFd > 0 && FD_ISSET(serverClientFd, &rfd)) {
        VLOG(3) << "ServerClientFd is selected";
        while (serverClientState->hasData()) {
          VLOG(3) << "ServerClientState has data";
          string packetTypeString;
          if (!serverClientState->readMessage(&packetTypeString)) {
            break;
          }
          char packetType = packetTypeString[0];
          if (packetType == et::PacketType::PORT_FORWARD_DATA ||
              packetType == et::PacketType::PORT_FORWARD_SOURCE_REQUEST ||
              packetType == et::PacketType::PORT_FORWARD_SOURCE_RESPONSE ||
              packetType == et::PacketType::PORT_FORWARD_DESTINATION_REQUEST ||
              packetType == et::PacketType::PORT_FORWARD_DESTINATION_RESPONSE) {
            portForwardHandler.handlePacket(packetType, serverClientState);
            continue;
          }
          switch (packetType) {
            case et::PacketType::OBSOLETE_PORT_FORWARD_DATA:
            case et::PacketType::OBSOLETE_PORT_FORWARD_REQUEST:
              // Legacy port forwarding requests/data are ignored.
              LOG(ERROR)
                  << "Received legacy port forwarding request.  Ignoring...";
              break;
            case et::PacketType::TERMINAL_BUFFER: {
              // Read from the server and write to our fake terminal
              et::TerminalBuffer tb =
                  serverClientState->readProto<et::TerminalBuffer>();
              VLOG(2) << "Got bytes from client: " << tb.buffer().length()
                      << " "
                      << serverClientState->getReader()->getSequenceNumber();
              char c = TERMINAL_BUFFER;
              terminalSocketHandler->writeAllOrThrow(terminalFd, &c,
                                                     sizeof(char), false);
              terminalSocketHandler->writeProto(terminalFd, tb, false);
              break;
            }
            case et::PacketType::KEEP_ALIVE: {
              // Echo keepalive back to client
              LOG(INFO) << "Got keep alive";
              char c = et::PacketType::KEEP_ALIVE;
              serverClientState->writeMessage(string(1, c));
              break;
            }
            case et::PacketType::TERMINAL_INFO: {
              LOG(INFO) << "Got terminal info";
              et::TerminalInfo ti =
                  serverClientState->readProto<et::TerminalInfo>();
              char c = TERMINAL_INFO;
              terminalSocketHandler->writeAllOrThrow(terminalFd, &c,
                                                     sizeof(char), false);
              terminalSocketHandler->writeProto(terminalFd, ti, false);
              break;
            }
            default:
              LOG(FATAL) << "Unknown packet type: " << int(packetType);
          }
        }
      }
    } catch (const runtime_error &re) {
      LOG(ERROR) << "Error: " << re.what();
      cerr << "Error: " << re.what();
      serverClientState->closeSocket();
      // If the client disconnects the session, it shouldn't end
      // because the client may be starting a new one.  TODO: Start a
      // timer which eventually kills the server.

      // run=false;
    }
  }
  {
    string id = serverClientState->getId();
    serverClientState.reset();
    globalServer->removeClient(id);
  }
}

void handleConnection(shared_ptr<ServerClientConnection> serverClientState) {
  InitialPayload payload = serverClientState->readProto<InitialPayload>();
  if (payload.jumphost()) {
    runJumpHost(serverClientState);
  } else {
    runTerminal(serverClientState);
  }
}

class TerminalServerHandler : public ServerConnectionHandler {
  virtual bool newClient(shared_ptr<ServerClientConnection> serverClientState) {
    lock_guard<std::mutex> guard(terminalThreadMutex);
    shared_ptr<thread> t =
        shared_ptr<thread>(new thread(handleConnection, serverClientState));
    terminalThreads.push_back(t);
    return true;
  }
};

void startServer() {
  std::shared_ptr<TcpSocketHandler> tcpSocketHandler(new TcpSocketHandler());
  std::shared_ptr<PipeSocketHandler> pipeSocketHandler(new PipeSocketHandler());

  LOG(INFO) << "Creating server";

  globalServer = shared_ptr<ServerConnection>(new ServerConnection(
      tcpSocketHandler, SocketEndpoint(FLAGS_port),
      shared_ptr<TerminalServerHandler>(new TerminalServerHandler())));
  terminalRouter = shared_ptr<UserTerminalRouter>(
      new UserTerminalRouter(pipeSocketHandler, ROUTER_FIFO_NAME));
  fd_set coreFds;
  int numCoreFds = 0;
  int maxCoreFd = 0;
  FD_ZERO(&coreFds);
  set<int> serverPortFds =
      tcpSocketHandler->getEndpointFds(SocketEndpoint(FLAGS_port));
  for (int i : serverPortFds) {
    FD_SET(i, &coreFds);
    maxCoreFd = max(maxCoreFd, i);
    numCoreFds++;
  }
  FD_SET(terminalRouter->getServerFd(), &coreFds);
  maxCoreFd = max(maxCoreFd, terminalRouter->getServerFd());
  numCoreFds++;

  while (true) {
    // Select blocks until there is something useful to do
    fd_set rfds = coreFds;
    int numFds = numCoreFds;
    int maxFd = maxCoreFd;
    timeval tv;

    if (numFds > FD_SETSIZE) {
      LOG(FATAL) << "Tried to select() on too many FDs";
    }

    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    int numFdsSet = select(maxFd + 1, &rfds, NULL, NULL, &tv);
    FATAL_FAIL(numFdsSet);
    if (numFdsSet == 0) {
      continue;
    }

    // We have something to do!
    for (int i : serverPortFds) {
      if (FD_ISSET(i, &rfds)) {
        globalServer->acceptNewConnection(i);
      }
    }
    if (FD_ISSET(terminalRouter->getServerFd(), &rfds)) {
      terminalRouter->acceptNewConnection(globalServer);
    }
  }

  globalServer->shutdown();
  halt = true;
  for (auto it : terminalThreads) {
    it->join();
  }
}

int main(int argc, char **argv) {
  // Version string need to be set before GFLAGS parse arguments
  SetVersionString(string(ET_VERSION));

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);

  if (FLAGS_logtostdout) {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  } else {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
    // Redirect std streams to a file
    LogHandler::setupStdStreams("/tmp/etserver");
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

  if (FLAGS_daemon) {
    if (::daemon(0, 0) == -1) {
      LOG(FATAL) << "Error creating daemon: " << strerror(errno);
    }
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

  startServer();

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
}
