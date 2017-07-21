#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "Headers.hpp"
#include "PortForwardServerHandler.hpp"
#include "ServerConnection.hpp"
#include "SocketUtils.hpp"
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
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#include <sys/ioctl.h>
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

DEFINE_int32(port, 0, "Port to listen on");
DEFINE_string(idpasskey, "",
              "If set, uses IPC to send a client id/key to the server daemon");
DEFINE_string(idpasskeyfile, "",
              "If set, uses IPC to send a client id/key to the server daemon "
              "from a file");
DEFINE_bool(daemon, false, "Daemonize the server");
DEFINE_string(cfgfile, "", "Location of the config file");

shared_ptr<ServerConnection> globalServer;
shared_ptr<UserTerminalRouter> terminalRouter;
vector<shared_ptr<thread>> terminalThreads;
mutex terminalThreadMutex;
bool halt = false;

void runTerminal(shared_ptr<ServerClientConnection> serverClientState) {
  // Whether the TE should keep running.
  bool run = true;

  // TE sends/receives data to/from the shell one char at a time.
  char b[BUF_SIZE];

  shared_ptr<SocketHandler> socketHandler = globalServer->getSocketHandler();
  unordered_map<int, shared_ptr<PortForwardServerHandler>> portForwardHandlers;
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

      vector<PortForwardData> dataToSend;
      for (auto& it : portForwardHandlers) {
        it.second->update(&dataToSend);
        if (it.second->getFd() == -1) {
          // Kill the handler and don't update the rest: we'll pick
          // them up later
          portForwardHandlers.erase(it.first);
          break;
        }
      }
      for (auto& pwd : dataToSend) {
        char c = PacketType::PORT_FORWARD_DATA;
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
          switch (packetType) {
            case et::PacketType::TERMINAL_BUFFER: {
              // Read from the server and write to our fake terminal
              et::TerminalBuffer tb =
                  serverClientState->readProto<et::TerminalBuffer>();
              const string& s = tb.buffer();
              // VLOG(2) << "Got bytes from client: " << s.length() << " " <<
              // serverClientState->getReader()->getSequenceNumber();
              FATAL_FAIL(writeAll(terminalFd, &s[0], s.length()));
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
              winsize tmpwin;
              tmpwin.ws_row = ti.row();
              tmpwin.ws_col = ti.column();
              tmpwin.ws_xpixel = ti.width();
              tmpwin.ws_ypixel = ti.height();
              ioctl(terminalFd, TIOCSWINSZ, &tmpwin);
              break;
            }
            case PacketType::PORT_FORWARD_REQUEST: {
              LOG(INFO) << "Got new port forward";
              PortForwardRequest pfr =
                  serverClientState->readProto<PortForwardRequest>();
              // Try ipv6 first
              int fd = socketHandler->connect("::1", pfr.port());
              if (fd == -1) {
                // Try ipv4 next
                fd = socketHandler->connect("127.0.0.1", pfr.port());
              }
              PortForwardResponse pfresponse;
              pfresponse.set_clientfd(pfr.fd());
              if (fd == -1) {
                pfresponse.set_error(strerror(errno));
              } else {
                int socketId = rand();
                int attempts = 0;
                while (portForwardHandlers.find(socketId) !=
                       portForwardHandlers.end()) {
                  socketId = rand();
                  attempts++;
                  if (attempts >= 100000) {
                    pfresponse.set_error("Could not find empty socket id");
                    break;
                  }
                }
                if (!pfresponse.has_error()) {
                  LOG(INFO)
                      << "Created socket/fd pair: " << socketId << ' ' << fd;
                  portForwardHandlers[socketId] =
                      shared_ptr<PortForwardServerHandler>(
                          new PortForwardServerHandler(socketHandler, fd,
                                                       socketId));
                  pfresponse.set_socketid(socketId);
                }
              }

              char c = PacketType::PORT_FORWARD_RESPONSE;
              serverClientState->writeMessage(string(1, c));
              serverClientState->writeProto(pfresponse);
              break;
            }
            case PacketType::PORT_FORWARD_DATA: {
              PortForwardData pwd =
                  serverClientState->readProto<PortForwardData>();
              LOG(INFO) << "Got data for socket: " << pwd.socketid();
              auto it = portForwardHandlers.find(pwd.socketid());
              if (it == portForwardHandlers.end()) {
                LOG(ERROR) << "Got data for a socket id that doesn't exist: "
                           << pwd.socketid();
              } else {
                if (pwd.has_closed()) {
                  LOG(INFO) << "Port forward socket closed: " << pwd.socketid();
                  it->second->close();
                  portForwardHandlers.erase(it);
                } else if (pwd.has_error()) {
                  // TODO: Probably need to do something better here
                  LOG(INFO)
                      << "Port forward socket errored: " << pwd.socketid();
                  it->second->close();
                  portForwardHandlers.erase(it);
                } else {
                  it->second->write(pwd.buffer());
                }
              }
              break;
            }
            default:
              LOG(FATAL) << "Unknown packet type: " << int(packetType) << endl;
          }
        }
      }
    } catch (const runtime_error& re) {
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
    shared_ptr<thread> t =
        shared_ptr<thread>(new thread(runTerminal, serverClientState));
    terminalThreads.push_back(t);
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
  UserTerminalHandler uth;
  uth.connectToRouter(idpasskey);
  cout << "IDPASSKEY:" << idpasskey << endl;
  if (::daemon(0, 0) == -1) {
    LOG(FATAL) << "Error creating daemon: " << strerror(errno);
  }
  string first_idpass_chars = idpasskey.substr(0, 10);
  string std_file = string("/tmp/etserver_terminal_") + first_idpass_chars;
  stdout = fopen(std_file.c_str(), "w+");
  setvbuf(stdout, NULL, _IOLBF, BUFSIZ);  // set to line buffering
  stderr = fopen(std_file.c_str(), "w+");
  setvbuf(stderr, NULL, _IOLBF, BUFSIZ);  // set to line buffering
  uth.run();
}

int main(int argc, char** argv) {
  gflags::SetVersionString(ET_VERSION);
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
        const char* portString = ini.GetValue("Networking", "Port", NULL);
        if (portString) {
          FLAGS_port = stoi(portString);
        }
      }
    } else {
      LOG(FATAL) << "Invalid config file: " << FLAGS_cfgfile;
    }
  }

  if (FLAGS_port == 0) {
    FLAGS_port = 2022;
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
