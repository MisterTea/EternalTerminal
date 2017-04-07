#include "IdPasskeyHandler.hpp"

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
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#if __APPLE__
#include <util.h>
#include <sys/ucred.h>
#elif __FreeBSD__
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <libutil.h>
#else
#include <pty.h>
#endif

#include "ServerConnection.hpp"

// TODO: This is ugly, fix later.
extern map<string, int64_t> idPidMap;
extern shared_ptr<et::ServerConnection> globalServer;

struct PeerInfo
{
    bool pidKnown;
    pid_t pid;
    bool uidKnown;
    uid_t uid;
    bool gidKnown;
    gid_t gid;
};

#define FIFO_NAME "/tmp/etserver.idpasskey.fifo"
void IdPasskeyHandler::runServer(bool* done) {
  int num, fd;

  unsigned int s2;
  sockaddr_un local, remote;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(fd);
  local.sun_family = AF_UNIX;  /* local is declared before socket() ^ */
  strcpy(local.sun_path, FIFO_NAME);
  unlink(local.sun_path);

  // Also set the accept socket as reusable
  {
    int flag = 1;
    FATAL_FAIL(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag,
                          sizeof(int)));
  }

  FATAL_FAIL(::bind(fd, (struct sockaddr *)&local, sizeof(sockaddr_un)));
  listen(fd, 5);
  chmod(local.sun_path, 0777);

  LOG(INFO) << "Listening to id/key FIFO";
  while(!(*done)) {
    printf("Waiting for a connection...\n");
    socklen_t t = sizeof(remote);
    if ((s2 = ::accept(fd, (struct sockaddr *)&remote, &t)) == -1) {
      FATAL_FAIL(-1);
    }

    LOG(INFO) << "Connected";

    PeerInfo peer = { false, 0, false, 0, false, 0 };

#if defined(SO_PEERCRED)
    struct ucred ucred;
    len = sizeof(struct ucred);
    FATAL_FAIL(getsockopt(s2, SOL_SOCKET, SO_PEERCRED, &ucred, &len));
    peer = { true, cred.pid, true, cred.uid, true, cred.gid };
#elif defined(LOCAL_PEERCRED)
    xucred cred;
    socklen_t credLen = sizeof(cred);
    FATAL_FAIL(getsockopt(s2, SOL_LOCAL, LOCAL_PEERCRED, &cred, &credLen));
    peer = { false, 0, true, cred.cr_uid, false, 0 };
#endif

    printf("Credentials from SO_PEERCRED: pid=%ld, euid=%ld, egid=%ld\n",
           (long) peer.pid, (long) peer.uid, (long) peer.gid);

    string buf;
    do {
      char c;
      if ((num = read(s2, &c, 1)) == -1) {
        LOG(FATAL) << "Error while reading from id/key FIFO: " << errno;
      } else if (num == 1) {
        if (c == '\0') {
          VLOG(1) << "Got idPasskey: " << buf << endl;
          size_t slashIndex = buf.find("/");
          if (slashIndex == string::npos) {
            LOG(ERROR) << "Invalid idPasskey id/key pair: " << buf;
          } else {
            string id = buf.substr(0, slashIndex);
            string key = buf.substr(slashIndex+1);
            idPidMap[id] = (int64_t)peer.uid;
            globalServer->addClientKey(id, key);
            break;
          }
          buf = "";
        } else {
          buf += c;
        }
      }
    } while (num > 0);
    close(s2);
  }
}

void IdPasskeyHandler::send(const string& idPasskey) {
  int fd;
  sockaddr_un remote;

  fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  FATAL_FAIL(fd);
  remote.sun_family = AF_UNIX;
  strcpy(remote.sun_path, FIFO_NAME);

  if (connect(fd, (struct sockaddr *) &remote, sizeof(sockaddr_un)) < 0) {
    close(fd);
    FATAL_FAIL(-1);
  }
  FATAL_FAIL(write(fd, &(idPasskey[0]), idPasskey.length()));
  close(fd);
}
