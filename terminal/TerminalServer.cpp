#include "ClientConnection.hpp"
#include "ConsoleUtils.hpp"
#include "CryptoHandler.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "Headers.hpp"
#include "ServerConnection.hpp"
#include "SocketUtils.hpp"
#include "UnixSocketHandler.hpp"
#include "IdPasskeyHandler.hpp"
#include "PortForwardServerHandler.hpp"

#include "simpleini/SimpleIni.h"

#include <errno.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <grp.h>

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <libutil.h>
#else
#include <pty.h>
#include <signal.h>
#endif

#ifdef WITH_SELINUX
#include <selinux/selinux.h>
#include <selinux/get_context_list.h>
#endif

#include "ETerminal.pb.h"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

map<string, int64_t> idPidMap;
shared_ptr<ServerConnection> globalServer;

void halt();

#define FAIL_FATAL(X)                                              \
  if ((X) == -1) {                                                 \
    LOG(FATAL) << "Error: (" << errno << "): " << strerror(errno); \
  }

DEFINE_int32(port, 0, "Port to listen on");
DEFINE_string(idpasskey, "", "If set, uses IPC to send a client id/key to the server daemon");
DEFINE_string(idpasskeyfile, "", "If set, uses IPC to send a client id/key to the server daemon from a file");
DEFINE_bool(daemon, false, "Daemonize the server");
DEFINE_string(cfgfile, "", "Location of the config file");

thread* idPasskeyListenerThread = NULL;
thread* finishClientMonitorThread = NULL;

int nextThreadId=0;
map<int, shared_ptr<thread>> terminalThreads;
vector<int> finishedThreads;
mutex terminalThreadMutex;

void finishClientMonitor() {
  while (true) {
    {
      lock_guard<std::mutex> guard(terminalThreadMutex);
      for (int id : finishedThreads) {
        terminalThreads[id]->join();
        terminalThreads.erase(id);
      }
      finishedThreads.clear();
    }
    sleep(1);
  }
}

void runTerminal(shared_ptr<ServerClientConnection> serverClientState,
                 int masterfd,
                 pid_t childPid,
                 int threadId) {
  string disconnectBuffer;

  // Whether the TE should keep running.
  bool run = true;

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (1024 * 1024)
  char b[BUF_SIZE];

  shared_ptr<SocketHandler> socketHandler = globalServer->getSocketHandler();
  unordered_map<int, shared_ptr<PortForwardServerHandler>> portForwardHandlers;

  while (run) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(masterfd, &rfd);
    int maxfd = masterfd;
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
      if (FD_ISSET(masterfd, &rfd)) {
        // Read from fake terminal and write to client
        memset(b, 0, BUF_SIZE);
        int rc = read(masterfd, b, BUF_SIZE);
        if (rc > 0) {
          //VLOG(2) << "Sending bytes from terminal: " << rc << " "
          //<< serverClientState->getWriter()->getSequenceNumber();
          char c = et::PacketType::TERMINAL_BUFFER;
          serverClientState->writeMessage(string(1, c));
          string s(b, rc);
          et::TerminalBuffer tb;
          tb.set_buffer(s);
          serverClientState->writeProto(tb);
        } else {
          LOG(INFO) << "Terminal session ended";
          siginfo_t childInfo;
          FATAL_FAIL(waitid(P_PID, childPid, &childInfo, WEXITED));
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
              //VLOG(2) << "Got bytes from client: " << s.length() << " " <<
              //serverClientState->getReader()->getSequenceNumber();
              FATAL_FAIL(writeAll(masterfd, &s[0], s.length()));
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
              ioctl(masterfd, TIOCSWINSZ, &tmpwin);
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
                while (portForwardHandlers.find(socketId) != portForwardHandlers.end()) {
                  socketId = rand();
                  attempts++;
                  if (attempts >= 100000) {
                    pfresponse.set_error("Could not find empty socket id");
                    break;
                  }
                }
                if (!pfresponse.has_error()) {
                  LOG(INFO) << "Created socket/fd pair: " << socketId << ' ' << fd;
                  portForwardHandlers[socketId] =
                      shared_ptr<PortForwardServerHandler>(
                          new PortForwardServerHandler(
                              socketHandler,
                              fd,
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
              PortForwardData pwd = serverClientState->readProto<PortForwardData>();
              LOG(INFO) << "Got data for socket: " << pwd.socketid();
              auto it = portForwardHandlers.find(pwd.socketid());
              if (it == portForwardHandlers.end()) {
                LOG(ERROR) << "Got data for a socket id that doesn't exist: " << pwd.socketid();
              } else {
                if (pwd.has_closed()) {
                  LOG(INFO) << "Port forward socket closed: " << pwd.socketid();
                  it->second->close();
                  portForwardHandlers.erase(it);
                } else if(pwd.has_error()) {
                  // TODO: Probably need to do something better here
                  LOG(INFO) << "Port forward socket errored: " << pwd.socketid();
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
    finishedThreads.push_back(threadId);
  }
}

void startTerminal(shared_ptr<ServerClientConnection> serverClientState,
                   InitialPayload payload) {
  const TerminalInfo& ti = payload.terminal();
  winsize win;
  win.ws_row = ti.row();
  win.ws_col = ti.column();
  win.ws_xpixel = ti.width();
  win.ws_ypixel = ti.height();
  for (const string& it : payload.environmentvar()) {
    size_t equalsPos = it.find("=");
    if (equalsPos == string::npos) {
      LOG(FATAL) << "Invalid environment variable";
    }
    string name = it.substr(0, equalsPos);
    string value = it.substr(equalsPos + 1);
    setenv(name.c_str(), value.c_str(), 1);
  }

  int masterfd;
  std::string terminal = getTerminal();

  pid_t pid = forkpty(&masterfd, NULL, NULL, &win);
  switch (pid) {
    case -1:
      FAIL_FATAL(pid);
    case 0: {
      // child
      string id = serverClientState->getId();
      if (idPidMap.find(id) == idPidMap.end()) {
        LOG(FATAL) << "Error: PID for ID not found";
      }
      int64_t pid = idPidMap[id];
      passwd* pwd = getpwuid(pid);

      // Set /proc/self/loginuid if it exists
      bool loginuidExists=false;
      {
        ifstream infile("/proc/self/loginuid");
        loginuidExists = infile.good();
      }
      if (loginuidExists) {
        ofstream outfile("/proc/self/loginuid");
        if (!outfile.is_open()) {
          LOG(ERROR) << "/proc/self/loginuid is not writable.";
        } else {
          outfile << pid;
          outfile.close();
        }
      }

      gid_t groups[65536];
      int ngroups = 65536;
#ifdef WITH_SELINUX
      char* sename = NULL;
      char* level = NULL;
      FATAL_FAIL(getseuserbyname(pwd->pw_name, &sename, &level));
      security_context_t user_ctx = NULL;
      FATAL_FAIL(get_default_context_with_level(sename, level, NULL, &user_ctx));
      setexeccon(user_ctx);
      free(sename);
      free(level);
#endif

#ifdef __APPLE__
      if (getgrouplist(pwd->pw_name, pwd->pw_gid, (int*)groups, &ngroups) == -1) {
        LOG(FATAL) << "User is part of more than 65536 groups!";
      }
#else
      if (getgrouplist(pwd->pw_name, pwd->pw_gid, groups, &ngroups) == -1) {
        LOG(FATAL) << "User is part of more than 65536 groups!";
      }
#endif

#ifdef setresgid
      FATAL_FAIL(setresgid(pwd->pw_gid, pwd->pw_gid, pwd->pw_gid));
#else // OS/X
      FATAL_FAIL(setregid(pwd->pw_gid, pwd->pw_gid));
#endif

#ifdef __APPLE__
      FATAL_FAIL(initgroups(pwd->pw_name, pwd->pw_gid));
#else
      FATAL_FAIL(::setgroups(ngroups, groups));
#endif

#ifdef setresuid
      FATAL_FAIL(setresuid(pwd->pw_uid, pwd->pw_uid, pwd->pw_uid));
#else // OS/X
      FATAL_FAIL(setreuid(pwd->pw_uid, pwd->pw_uid));
#endif
      if (pwd->pw_shell) {
        terminal = pwd->pw_shell;
      }
      setenv("SHELL", terminal.c_str(), 1);

      const char *homedir = pwd->pw_dir;
      setenv("HOME", homedir, 1);
      setenv("USER", pwd->pw_name, 1);
      setenv("LOGNAME", pwd->pw_name, 1);
      setenv("PATH", "/usr/local/bin:/bin:/usr/bin", 1);
      chdir(pwd->pw_dir);

      VLOG(1) << "Child process " << terminal << endl;
      execl(terminal.c_str(), terminal.c_str(), "--login", NULL);
      exit(0);
      break;
    }
    default: {
      // parent
      VLOG(1) << "pty opened " << masterfd << endl;
      lock_guard<std::mutex> guard(terminalThreadMutex);
      shared_ptr<thread> t = shared_ptr<thread>(new thread(runTerminal, serverClientState, masterfd, pid, nextThreadId));
      terminalThreads.insert(pair<int,shared_ptr<thread>>(nextThreadId, t));
      nextThreadId++;
      break;
    }
  }
}

class TerminalServerHandler : public ServerConnectionHandler {
  virtual bool newClient(shared_ptr<ServerClientConnection> serverClientState) {
    InitialPayload payload = serverClientState->readProto<InitialPayload>();
    startTerminal(serverClientState, payload);
    return true;
  }
};

bool doneListening=false;

int main(int argc, char** argv) {
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
    idpasskey += '\0';
    IdPasskeyHandler::send(idpasskey);

    return 0;
  }

  if (FLAGS_daemon) {
    if (::daemon(0,0) == -1) {
      LOG(FATAL) << "Error creating daemon: " << strerror(errno);
    }
    stdout = fopen("/tmp/etserver_err", "w+");
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);  // set to line buffering
    stderr = fopen("/tmp/etserver_err", "w+");
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);  // set to line buffering
  }

  std::shared_ptr<UnixSocketHandler> serverSocket(new UnixSocketHandler());

  LOG(INFO) << "Creating server";

  globalServer = shared_ptr<ServerConnection>(new ServerConnection(
      serverSocket, FLAGS_port,
      shared_ptr<TerminalServerHandler>(new TerminalServerHandler())));
  idPasskeyListenerThread = new thread(IdPasskeyHandler::runServer, &doneListening);
  finishClientMonitorThread = new thread(finishClientMonitor);
  globalServer->run();
}

void halt() {
  LOG(INFO) << "Shutting down server" << endl;
  doneListening = true;
  globalServer->close();
  LOG(INFO) << "Waiting for server to finish" << endl;
  sleep(3);
  exit(0);
}
