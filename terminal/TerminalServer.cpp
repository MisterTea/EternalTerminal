#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "Headers.hpp"
#include "PortForwardHandler.hpp"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "SystemUtils.hpp"
#include "UnixSocketHandler.hpp"
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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
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
const int KEEP_ALIVE_DURATION = 7;

DEFINE_int32(port, 0, "Port to listen on");
DEFINE_string(idpasskey, "",
              "If set, uses IPC to send a client id/key to the server daemon");
DEFINE_string(idpasskeyfile, "",
              "If set, uses IPC to send a client id/key to the server daemon "
              "from a file");
DEFINE_bool(daemon, false, "Daemonize the server");
DEFINE_string(cfgfile, "", "Location of the config file");
DEFINE_bool(jump, false,
            "If set, forward all packets between client and dst terminal");
DEFINE_string(dsthost, "", "Must be set if jump is set to true");
DEFINE_int32(dstport, 2022, "Must be set if jump is set to true");

shared_ptr<ServerConnection> globalServer;
shared_ptr<UserTerminalRouter> terminalRouter;
vector<shared_ptr<thread>> terminalThreads;
mutex terminalThreadMutex;
bool halt = false;
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

void setGlogVerboseLevel(int vlevel) { FLAGS_v = vlevel; }

void setGlogMinLogLevel(int level) { FLAGS_minloglevel = level; }

void setGlogFile(string filename) {
  google::SetLogDestination(google::INFO, (filename + ".INFO.").c_str());
  google::SetLogDestination(google::WARNING, (filename + ".WARNING.").c_str());
  google::SetLogDestination(google::ERROR, (filename + ".ERROR.").c_str());
}

void setDaemonLogFile(string idpasskey, string daemonType) {
  string first_idpass_chars = idpasskey.substr(0, 10);
  string std_file =
      string("/tmp/etserver_") + daemonType + "_" + first_idpass_chars;
  stdout = fopen(std_file.c_str(), "w+");
  setvbuf(stdout, NULL, _IOLBF, BUFSIZ);  // set to line buffering
  stderr = fopen(std_file.c_str(), "w+");
  setvbuf(stderr, NULL, _IOLBF, BUFSIZ);  // set to line buffering
  setGlogFile(std_file);
}

void runJumpHost(shared_ptr<ServerClientConnection> serverClientState) {
  bool run = true;

  bool b[BUF_SIZE];
  int terminalFd = terminalRouter->getFd(serverClientState->getId());

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
          string message = RawSocketUtils::readMessage(terminalFd);
          serverClientState->writeMessage(message);
        } catch (const std::runtime_error &ex) {
          LOG(INFO) << "Terminal session ended";
          run = false;
          globalServer->removeClient(serverClientState->getId());
          break;
        }
      }

      if (serverClientFd > 0 && FD_ISSET(serverClientFd, &rfd)) {
        while (serverClientState->hasData()) {
          string message;
          if (!serverClientState->readMessage(&message)) {
            break;
          }
          RawSocketUtils::writeMessage(terminalFd, message);
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
  // Whether the TE should keep running.
  bool run = true;

  // TE sends/receives data to/from the shell one char at a time.
  char b[BUF_SIZE];

  shared_ptr<SocketHandler> socketHandler = globalServer->getSocketHandler();
  PortForwardHandler portForwardHandler(socketHandler);
  int terminalFd = terminalRouter->getFd(serverClientState->getId());

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
          // VLOG(2) << "Sending bytes from terminal: " << rc << " "
          //<< serverClientState->getWriter()->getSequenceNumber();
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

      vector<PortForwardData> dataToSend = portForwardHandler.update();
      for (auto &pwd : dataToSend) {
        char c = PacketType::PORT_FORWARD_DS_DATA;
        string headerString(1, c);
        serverClientState->writeMessage(headerString);
        serverClientState->writeProto(pwd);
      }

      if (serverClientFd > 0 && FD_ISSET(serverClientFd, &rfd)) {
        while (serverClientState->hasData()) {
          string packetTypeString;
          if (!serverClientState->readMessage(&packetTypeString)) {
            break;
          }
          char packetType = packetTypeString[0];
          if (packetType == et::PacketType::PORT_FORWARD_SD_DATA
              || packetType == et::PacketType::PORT_FORWARD_DS_DATA
              || packetType == et::PacketType::PORT_FORWARD_SOURCE_REQUEST
              || packetType == et::PacketType::PORT_FORWARD_SOURCE_RESPONSE
              || packetType == et::PacketType::PORT_FORWARD_DESTINATION_REQUEST
              || packetType == et::PacketType::PORT_FORWARD_DESTINATION_RESPONSE
              ) {
            portForwardHandler.handlePacket(packetType, serverClientState);
            continue;
          }
          switch (packetType) {
            case et::PacketType::TERMINAL_BUFFER: {
              // Read from the server and write to our fake terminal
              et::TerminalBuffer tb =
                  serverClientState->readProto<et::TerminalBuffer>();
              // VLOG(2) << "Got bytes from client: " << s.length() << " " <<
              // serverClientState->getReader()->getSequenceNumber();
              char c = TERMINAL_BUFFER;
              RawSocketUtils::writeAll(terminalFd, &c, sizeof(char));
              RawSocketUtils::writeProto(terminalFd, tb);
              break;
            }
            case et::PacketType::KEEP_ALIVE: {
              // Echo keepalive back to client
              VLOG(1) << "Got keep alive";
              char c = et::PacketType::KEEP_ALIVE;
              serverClientState->writeMessage(string(1, c));
              break;
            }
            case et::PacketType::TERMINAL_INFO: {
              VLOG(1) << "Got terminal info";
              et::TerminalInfo ti =
                  serverClientState->readProto<et::TerminalInfo>();
              char c = TERMINAL_INFO;
              RawSocketUtils::writeAll(terminalFd, &c, sizeof(char));
              RawSocketUtils::writeProto(terminalFd, ti);
              break;
            }
            default:
              LOG(FATAL) << "Unknown packet type: " << int(packetType) << endl;
          }
        }
      }
    } catch (const runtime_error &re) {
      LOG(ERROR) << "Error: " << re.what();
      cerr << "Error: " << re.what();
      serverClientState->closeSocket();
      // If the client disconnects the session, it shuoldn't end
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

class TerminalServerHandler : public ServerConnectionHandler {
  virtual bool newClient(shared_ptr<ServerClientConnection> serverClientState) {
    InitialPayload payload = serverClientState->readProto<InitialPayload>();
    lock_guard<std::mutex> guard(terminalThreadMutex);
    if (payload.jumphost()) {
      shared_ptr<thread> t =
          shared_ptr<thread>(new thread(runJumpHost, serverClientState));
      terminalThreads.push_back(t);
    } else {
      shared_ptr<thread> t =
          shared_ptr<thread>(new thread(runTerminal, serverClientState));
      terminalThreads.push_back(t);
    }
    return true;
  }
};

void startServer() {
  std::shared_ptr<UnixSocketHandler> socketHandler(new UnixSocketHandler());

  LOG(INFO) << "Creating server";

  globalServer = shared_ptr<ServerConnection>(new ServerConnection(
      socketHandler, FLAGS_port,
      shared_ptr<TerminalServerHandler>(new TerminalServerHandler())));
  terminalRouter = shared_ptr<UserTerminalRouter>(new UserTerminalRouter());
  fd_set coreFds;
  int numCoreFds = 0;
  int maxCoreFd = 0;
  FD_ZERO(&coreFds);
  set<int> serverPortFds = socketHandler->getPortFds(FLAGS_port);
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

  globalServer->close();
  halt = true;
  for (auto it : terminalThreads) {
    it->join();
  }
}

void startUserTerminal() {
  string idpasskey = getIdpasskey();
  UserTerminalHandler uth;
  uth.connectToRouter(idpasskey);
  cout << "IDPASSKEY:" << idpasskey << endl;
  if (::daemon(0, 0) == -1) {
    LOG(FATAL) << "Error creating daemon: " << strerror(errno);
  }
  setDaemonLogFile(idpasskey, "terminal");
  uth.run();
}

void startJumpHostClient() {
  string idpasskey = getIdpasskey();
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
  sockaddr_un remote;

  int routerFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(routerFd);
  remote.sun_family = AF_UNIX;
  strcpy(remote.sun_path, ROUTER_FIFO_NAME);

  if (connect(routerFd, (struct sockaddr *)&remote, sizeof(sockaddr_un)) < 0) {
    close(routerFd);
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

  RawSocketUtils::writeMessage(routerFd, idpasskey);
  ConfigParams config = RawSocketUtils::readProto<ConfigParams>(routerFd);
  setGlogVerboseLevel(config.vlevel());
  setGlogMinLogLevel(config.minloglevel());

  InitialPayload payload;

  shared_ptr<SocketHandler> jumpclientSocket(new UnixSocketHandler());
  shared_ptr<ClientConnection> jumpclient = shared_ptr<ClientConnection>(
      new ClientConnection(jumpclientSocket, host, port, id, passkey));

  int connectFailCount = 0;
  while (true) {
    try {
      jumpclient->connect();
      jumpclient->writeProto(payload);
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
  VLOG(1) << "JumpClient created with id: " << jumpclient->getId() << endl;

  bool run = true;
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
        keepaliveTime = time(NULL) + KEEP_ALIVE_DURATION;
        if (jumpClientFd < 0) {
          LOG(INFO) << "User comes back, reconnecting";
          jumpclient->closeSocket();
          sleep(3);
          continue;
        } else {
          string s = RawSocketUtils::readMessage(routerFd);
          jumpclient->writeMessage(s);
        }
      }
      // forward DST terminal -> local router
      if (jumpClientFd > 0 && FD_ISSET(jumpClientFd, &rfd)) {
        while (jumpclient->hasData()) {
          string receivedMessage;
          jumpclient->readMessage(&receivedMessage);
          RawSocketUtils::writeMessage(routerFd, receivedMessage);
        }
      }
      // src disconnects, close jump -> dst
      if (jumpClientFd > 0 && keepaliveTime < time(NULL)) {
        LOG(INFO) << "Jumpclient idle, killing connection";
        jumpclient->Connection::closeSocket();
      }
    } catch (const runtime_error &re) {
      LOG(ERROR) << "Error: " << re.what() << endl;
      cout << "Connection closing because of error: " << re.what() << endl;
      run = false;
    }
  }
  LOG(ERROR) << "Jumpclient shutdown";
}

int main(int argc, char **argv) {
  SetVersionString(string(ET_VERSION));
  ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  FLAGS_logbufsecs = 0;
  FLAGS_logbuflevel = google::GLOG_INFO;
  srand(1);

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
        setGlogVerboseLevel(atoi(vlevel));
      }
      // read silent setting
      const char *silent = ini.GetValue("Debug", "silent", NULL);
      if (silent && atoi(silent) != 0) {
        setGlogVerboseLevel(0);
        setGlogMinLogLevel(2);
      }
    } else {
      LOG(FATAL) << "Invalid config file: " << FLAGS_cfgfile;
    }
  }

  if (FLAGS_port == 0) {
    FLAGS_port = 2022;
  }

  if (FLAGS_jump) {
    startJumpHostClient();
    return 0;
  }

  if (FLAGS_idpasskey.length() > 0 || FLAGS_idpasskeyfile.length() > 0) {
    startUserTerminal();
    return 0;
  }

  if (FLAGS_daemon) {
    if (::daemon(0, 0) == -1) {
      LOG(FATAL) << "Error creating daemon: " << strerror(errno);
    }
    stdout = fopen("/tmp/etserver_err", "w+");
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);  // set to line buffering
    stderr = fopen("/tmp/etserver_err", "w+");
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);  // set to line buffering
  }

  startServer();
}
