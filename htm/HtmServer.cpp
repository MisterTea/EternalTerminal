#include "HtmServer.hpp"

#include "HTM.pb.h"

#include "HtmHeaderCodes.hpp"
#include "LogHandler.hpp"
#include "MultiplexerState.hpp"
#include "RawSocketUtils.hpp"

namespace et {
HtmServer::HtmServer() : IpcPairServer(HtmServer::getPipeName()) {}

void HtmServer::run() {
  while (true) {
    if (endpointFd < 0) {
      sleep(1);
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
        LOG(ERROR) << "READING FROM STDIN";
        RawSocketUtils::readAll(endpointFd, (char *)&header, 1);
        LOG(ERROR) << "Got message header: " << int(header);
        int32_t length;
        RawSocketUtils::readB64(endpointFd, (char *)&length, 4);
        LOG(ERROR) << "READ LENGTH: " << length;
        switch (header) {
          case INSERT_KEYS: {
            string uid = string(UUID_LENGTH, '0');
            RawSocketUtils::readAll(endpointFd, &uid[0], uid.length());
            length -= uid.length();
            LOG(ERROR) << "READING FROM " << uid << ":" << length;
            string data(length, '\0');
            RawSocketUtils::readAll(endpointFd, &data[0], length);
            LOG(ERROR) << "READ FROM " << uid << ":" << data << " " << length;
            state.appendData(uid, data);
            break;
          }
          case NEW_TAB: {
            string tabId = string(UUID_LENGTH, '0');
            RawSocketUtils::readAll(endpointFd, &tabId[0], tabId.length());
            string paneId = string(UUID_LENGTH, '0');
            RawSocketUtils::readAll(endpointFd, &paneId[0], paneId.length());
            state.newTab(tabId, paneId);
            break;
          }
          case NEW_SPLIT: {
            string sourceId = string(UUID_LENGTH, '0');
            RawSocketUtils::readAll(endpointFd, &sourceId[0],
                                    sourceId.length());
            string paneId = string(UUID_LENGTH, '0');
            RawSocketUtils::readAll(endpointFd, &paneId[0], paneId.length());
            char vertical;
            RawSocketUtils::readAll(endpointFd, &vertical, 1);
            state.newSplit(sourceId, paneId, vertical == '1');
            break;
          }
          case RESIZE_PANE: {
            int32_t cols;
            RawSocketUtils::readB64(endpointFd, (char *)&cols, 4);
            int32_t rows;
            RawSocketUtils::readB64(endpointFd, (char *)&rows, 4);
            string paneId = string(UUID_LENGTH, '0');
            RawSocketUtils::readAll(endpointFd, &paneId[0], paneId.length());
            state.resizePane(paneId, cols, rows);
            break;
          }
          case CLIENT_CLOSE_PANE: {
            string paneId = string(UUID_LENGTH, '0');
            RawSocketUtils::readAll(endpointFd, &paneId[0], paneId.length());
            LOG(INFO) << "CLOSING PANE: " << paneId;
            state.closePane(paneId);
            break;
          }
          default: {
            LOG(FATAL) << "Got unknown packet header: " << int(header);
          }
        }
      }

      state.update(endpointFd);
    } catch (std::runtime_error &re) {
      LOG(ERROR) << re.what();
      closeEndpoint();
    }
  }
}

void HtmServer::recover() {
  // Start by writing ESC + [###q to put the client terminal into HTM mode
  char buf[] = {
      0x1b, 0x5b, '#', '#', '#', 'q',
  };
  RawSocketUtils::writeAll(endpointFd, buf, sizeof(buf));
  fflush(stdout);
  // Sleep to make sure the client can process the escape code
  usleep(10 * 1000);

  // Send the state

  LOG(ERROR) << "Starting terminal";

  {
    unsigned char header = INIT_STATE;
    string jsonString;
    auto status = google::protobuf::util::MessageToJsonString(
        state.getStateProto(), &jsonString);
    VLOG(1) << "STATUS: " << status;
    int32_t length = jsonString.length();
    VLOG(1) << "SENDING INIT: " << jsonString;
    RawSocketUtils::writeAll(endpointFd, (const char *)&header, 1);
    RawSocketUtils::writeB64(endpointFd, (const char *)&length, 4);
    RawSocketUtils::writeAll(endpointFd, &jsonString[0], jsonString.length());
  }

  state.sendTerminalBuffers(endpointFd);
}

string HtmServer::getPipeName() {
  uid_t myuid = getuid();
  return string("/tmp/htm.") + to_string(myuid) + string(".ipc");
}
}  // namespace et
