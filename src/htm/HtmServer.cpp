#include "HtmServer.hpp"

#include "ControlCommands.hpp"
#include "PlatformUtils.hpp"

namespace et {
HtmServer::HtmServer(shared_ptr<SocketHandler> _socketHandler,
                     const SocketEndpoint& endpoint)
    : IpcPairServer(_socketHandler, endpoint),
      skipLfAfterCr(false),
      running(true),
      paneDumpRequested(false) {
  state.setWriter(&writer);
}

void HtmServer::handleClientData() {
  char buf[4096];
  int rc = socketHandler->read(endpointFd, buf, sizeof(buf));
  if (rc <= 0) {
    LOG(INFO) << "Client disconnect";
    closeEndpoint();
    writer.clearSocket();
    return;
  }
  lineBuf.append(buf, static_cast<size_t>(rc));
  // iTerm2's tmux gateway writes CR by default. Real tmux sees LF because the
  // PTY has ICRNL; htm puts stdin in raw mode, so split on CR and LF here.
  if (skipLfAfterCr) {
    if (!lineBuf.empty() && lineBuf[0] == '\n') {
      lineBuf.erase(0, 1);
    }
    skipLfAfterCr = false;
  }
  size_t pos = 0;
  while (true) {
    size_t termLen = 0;
    size_t nl = string::npos;
    for (size_t i = pos; i < lineBuf.size(); i++) {
      if (lineBuf[i] == '\n') {
        nl = i;
        termLen = 1;
        break;
      }
      if (lineBuf[i] == '\r') {
        nl = i;
        if (i + 1 < lineBuf.size() && lineBuf[i + 1] == '\n') {
          termLen = 2;
        } else {
          termLen = 1;
          skipLfAfterCr = (i + 1 == lineBuf.size());
        }
        break;
      }
    }
    if (nl == string::npos) {
      if (pos > 0) {
        lineBuf.erase(0, pos);
      }
      break;
    }
    string line = lineBuf.substr(pos, nl - pos);
    pos = nl + termLen;
    processLine(line);
    if (endpointFd < 0) {
      lineBuf.clear();
      return;
    }
  }
}

void HtmServer::processLine(const string& line) {
  vector<string> commands = splitControlCommandList(line);
  if (commands.empty()) {
    commands.push_back(line);
  }
  for (const string& cmd : commands) {
    LOG(INFO) << "control command: " << cmd;
    ControlAction action = executeControlCommand(&state, &writer, cmd);
    if (action == ControlAction::Error) {
      // iTerm2 auto-fails the rest of a `;` command list after %error.
      break;
    }
    if (action == ControlAction::Detach) {
      endControlSession();
      return;
    }
    if (action == ControlAction::KillServer) {
      endControlSession();
      running.store(false);
      return;
    }
  }
}

void HtmServer::run() {
  while (running.load()) {
    if (endpointFd < 0) {
      writePaneDumpIfRequested();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      try {
        pollAccept();
      } catch (const std::exception& re) {
        STERROR << re.what();
        try {
          closeEndpoint();
        } catch (const std::exception& closeEx) {
          LOG(INFO) << "closeEndpoint after accept/recover: " << closeEx.what();
        }
        writer.clearSocket();
      }
      continue;
    }

    try {
      // A hangup also reports readable; handleClientData() then sees EOF.
      if (waitOnSocketData(endpointFd, 0, 10000)) {
        handleClientData();
      }
      if (endpointFd > 0) {
        state.pollOutput();
        writePaneDumpIfRequested();
        // Shell-exit of the last pane does not go through kill-pane; still
        // end control mode the way tmux -CC does when no windows remain.
        if (state.empty()) {
          endControlSession();
          running.store(false);
        }
      }
    } catch (const std::exception& re) {
      try {
        closeEndpoint();
      } catch (const std::exception& closeEx) {
        LOG(INFO) << "closeEndpoint after disconnect: " << closeEx.what();
      }
      writer.clearSocket();
      LOG(INFO) << "Client disconnect: " << re.what();
    }
  }
  try {
    closeEndpoint();
  } catch (const std::exception& closeEx) {
    LOG(INFO) << "closeEndpoint on shutdown: " << closeEx.what();
  }
  writer.clearSocket();
  state.stopAll();
}

void HtmServer::recover() {
  writer.setSocket(socketHandler, endpointFd);
  lineBuf.clear();
  skipLfAfterCr = false;
  // tmux -CC prints an empty server-originated block on attach. iTerm2 will not
  // send refresh-client / list-windows until it sees this %end (flags 0).
  writer.beginServerOriginated();
  writer.end();
  state.attachNotifications();
}

string HtmServer::getPaneDumpPath() {
  return string(GetTempDirectory() + "htm.") + GetHtmIpcUser() +
         string(".panes");
}

void HtmServer::writePaneDumpIfRequested() {
  if (!paneDumpRequested.exchange(false)) {
    return;
  }
  writePaneDumpFile(getPaneDumpPath(), state.dumpAllPanesText());
}

bool HtmServer::writePaneDumpFile(const string& path, const string& body) {
  const string tmp = path + ".tmp";
#ifdef WIN32
  FILE* fp = fopen(tmp.c_str(), "w");
#else
  // The dump holds pane contents at a predictable path in the shared temp
  // directory, so create it owner-only without following planted links.
  const int fd = CreatePrivateFile(tmp);
  FILE* fp = fd >= 0 ? fdopen(fd, "w") : nullptr;
  if (fd >= 0 && !fp) {
    ::close(fd);
    ::remove(tmp.c_str());
  }
#endif
  if (!fp) {
    LOG(WARNING) << "pane dump open failed: " << tmp;
    return false;
  }
  const bool written = fwrite(body.data(), 1, body.size(), fp) == body.size();
  if (fclose(fp) != 0 || !written) {
    LOG(WARNING) << "pane dump write failed: " << tmp;
    ::remove(tmp.c_str());
    return false;
  }
  std::error_code renameError;
  fs::rename(tmp, path, renameError);
  if (renameError) {
    LOG(WARNING) << "pane dump rename failed: " << renameError.message();
    ::remove(tmp.c_str());
    return false;
  }
  return true;
}

string HtmServer::getShutdownEventName() {
  return string("Local\\EternalTerminal.HtmShutdown.") + GetHtmIpcUser();
}

string HtmServer::getPaneDumpEventName() {
  return string("Local\\EternalTerminal.HtmPaneDump.") + GetHtmIpcUser();
}

void HtmServer::endControlSession() {
  const int64_t peer = controlClientPid(endpointFd);
  writer.tryNotify("%exit");
  closeEndpoint();
  writer.clearSocket();
  reapControlClient(peer);
}
}  // namespace et
