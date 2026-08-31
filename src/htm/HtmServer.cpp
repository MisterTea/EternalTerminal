#include "HtmServer.hpp"

#include <cstdint>

#include "HtmHeaderCodes.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "base64.h"

namespace et {
HtmServer::HtmServer(shared_ptr<SocketHandler> _socketHandler,
                     const SocketEndpoint& endpoint)
    : IpcPairServer(_socketHandler, endpoint),
      state(_socketHandler),
      running(true) {}

void HtmServer::run() {
  while (running.load()) {
    if (endpointFd < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      pollAccept();
      continue;
    }

    try {
      char header;
      // Data structures needed for select() and
      // non-blocking I/O.
      fd_set rfd;
      timeval tv;

      FD_ZERO(&rfd);
      FD_SET(endpointFd, &rfd);
      tv.tv_sec = 0;
      tv.tv_usec = 10000;
      select(endpointFd + 1, &rfd, NULL, NULL, &tv);

      // Check for data to receive; the received
      // data includes also the data previously sent
      // on the same master descriptor (line 90).
      if (FD_ISSET(endpointFd, &rfd)) {
        VLOG(1) << "READING FROM STDIN";
        socketHandler->readAll(endpointFd, (char*)&header, 1, false);
        VLOG(1) << "Got message header: " << int(header);
        if (header == SESSION_END) {
          // SESSION_END is a 1-byte packet with no length field. Reading a
          // length here races with client teardown and hangs the daemon.
          LOG(INFO) << "Client sent SESSION_END";
          closeEndpoint();
          continue;
        }
        if (header != INSERT_KEYS && header != INSERT_DEBUG_KEYS &&
            header != NEW_TAB && header != NEW_SPLIT && header != RESIZE_PANE &&
            header != CLIENT_CLOSE_PANE) {
          // A stray keystroke (for example a leftover newline while Hyper is
          // still switching into HTM mode) must not be parsed as a length
          // prefix; that used to abort the session.
          LOG(INFO) << "Unexpected HTM header, disconnecting: " << int(header);
          closeEndpoint();
          continue;
        }
        int32_t length;
        socketHandler->readB64(endpointFd, (char*)&length, 4);
        VLOG(1) << "READ LENGTH: " << length;
        switch (header) {
          case INSERT_KEYS: {
            string uid = string(UUID_LENGTH, '0');
            socketHandler->readAll(endpointFd, &uid[0], uid.length(), false);
            length -= uid.length();
            VLOG(1) << "READING FROM " << uid << ":" << length;
            string data;
            socketHandler->readB64EncodedLength(endpointFd, &data, length);
            VLOG(1) << "READ FROM " << uid << ":" << data << " " << length;
            state.appendData(uid, data);
            break;
          }
          case INSERT_DEBUG_KEYS: {
            LOG(INFO) << "READING DEBUG: " << length;
            string data(length, '\0');
            socketHandler->readAll(endpointFd, &data[0], length, false);
            if (data[0] == 'x') {
              // x key pressed, exit
              running.store(false);
            }
            if (data[0] == 27) {
              // escape key pressed, disconnect
              LOG(INFO) << "CLOSING ENDPOINT";
              closeEndpoint();
            }
            if (data[0] == 'd') {
              string jsonString = state.toJsonString();
              LOG(INFO) << "Current State: " << jsonString;
            }
            break;
          }
          case NEW_TAB: {
            string tabId = string(UUID_LENGTH, '0');
            socketHandler->readAll(endpointFd, &tabId[0], tabId.length(),
                                   false);
            string paneId = string(UUID_LENGTH, '0');
            socketHandler->readAll(endpointFd, &paneId[0], paneId.length(),
                                   false);
            state.newTab(tabId, paneId);
            break;
          }
          case NEW_SPLIT: {
            string sourceId = string(UUID_LENGTH, '0');
            socketHandler->readAll(endpointFd, &sourceId[0], sourceId.length(),
                                   false);
            string paneId = string(UUID_LENGTH, '0');
            socketHandler->readAll(endpointFd, &paneId[0], paneId.length(),
                                   false);
            char vertical;
            socketHandler->readAll(endpointFd, &vertical, 1, false);
            state.newSplit(sourceId, paneId, vertical == '1');
            break;
          }
          case RESIZE_PANE: {
            int32_t cols;
            socketHandler->readB64(endpointFd, (char*)&cols, 4);
            int32_t rows;
            socketHandler->readB64(endpointFd, (char*)&rows, 4);
            string paneId = string(UUID_LENGTH, '0');
            socketHandler->readAll(endpointFd, &paneId[0], paneId.length(),
                                   false);
            state.resizePane(paneId, cols, rows);
            break;
          }
          case CLIENT_CLOSE_PANE: {
            string paneId = string(UUID_LENGTH, '0');
            socketHandler->readAll(endpointFd, &paneId[0], paneId.length(),
                                   false);
            LOG(INFO) << "CLOSING PANE: " << paneId;
            state.closePane(paneId);
            if (state.numPanes() == 0) {
              // No panes left
              running.store(false);
            }
            break;
          }
          default: {
            throw std::runtime_error(string("Got unknown packet header: ") +
                                     to_string(int(header)));
          }
        }
      }

      if (endpointFd > 0) {
        state.update(endpointFd);
      }
    } catch (const std::exception& re) {
      STERROR << re.what();
      try {
        closeEndpoint();
      } catch (const std::exception& closeEx) {
        LOG(INFO) << "closeEndpoint after disconnect: " << closeEx.what();
      }
    }
  }
  try {
    closeEndpoint();
  } catch (const std::exception& closeEx) {
    LOG(INFO) << "closeEndpoint on shutdown: " << closeEx.what();
  }
}

void HtmServer::sendDebug(const string& msg) {
  LOG(INFO) << "SENDING DEBUG LOG: " << msg;
  unsigned char header = DEBUG_LOG;
  int32_t length = Base64::EncodedLength(msg);
  socketHandler->writeAllOrThrow(endpointFd, (const char*)&header, 1, false);
  socketHandler->writeB64(endpointFd, (const char*)&length, 4);
  socketHandler->writeB64(endpointFd, &msg[0], msg.length());
}

void HtmServer::recover() {
  // Start by writing ESC + [###q to put the client terminal into HTM mode
  char buf[] = {
      0x1b, 0x5b, '#', '#', '#', 'q',
  };
  socketHandler->writeAllOrThrow(endpointFd, buf, sizeof(buf), false);
  fflush(stdout);

  // Send the state immediately so the init sequence and first packets can
  // arrive in the same client read, avoiding a race with 16ms PTY batching.
  LOG(INFO) << "Starting terminal";

  sendDebug("Initializing HTM, please wait...\n\r");

  {
    unsigned char header = INIT_STATE;
    string jsonString = state.toJsonString();
    int32_t length = jsonString.length();
    VLOG(1) << "SENDING INIT: " << jsonString;
    VLOG(1) << "SENDING INIT: " << length;
    socketHandler->writeAllOrThrow(endpointFd, (const char*)&header, 1, false);
    socketHandler->writeB64(endpointFd, (const char*)&length, 4);
    socketHandler->writeAllOrThrow(endpointFd, &jsonString[0],
                                   jsonString.length(), false);
  }

  state.sendTerminalBuffers(endpointFd);

  sendDebug(
      "HTM initialized.\n\rPress escape in this terminal to "
      "disconnect.\n\rPress x in this terminal to shut down HTM\n\r");
}

string HtmServer::getPipeName() {
  return string(GetTempDirectory() + "htm.") + GetHtmIpcUser() + string(".ipc");
}
}  // namespace et
