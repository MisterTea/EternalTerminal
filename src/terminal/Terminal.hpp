#ifndef __TERMINAL_HPP__
#define __TERMINAL_HPP__

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

#include "ClientConnection.hpp"
#include "CryptoHandler.hpp"
#include "DaemonCreator.hpp"
#include "Headers.hpp"
#include "LogHandler.hpp"
#include "ParseConfigFile.hpp"
#include "PortForwardHandler.hpp"
#include "PsuedoUserTerminal.hpp"
#include "ServerConnection.hpp"
#include "SystemUtils.hpp"
#include "TcpSocketHandler.hpp"
#include "UserTerminalHandler.hpp"
#include "UserTerminalRouter.hpp"

#include "simpleini/SimpleIni.h"

namespace et {
void startUserTerminal(shared_ptr<SocketHandler> ipcSocketHandler,
                       shared_ptr<UserTerminal> term, string idpasskey,
                       bool noratelimit){};

}  // namespace et

#endif