#include "BackedReader.hpp"

namespace et {
BackedReader::BackedReader(std::shared_ptr<SocketHandler> socketHandler_,  //
                           std::shared_ptr<CryptoHandler> cryptoHandler_,  //
                           int socketFd_)
    : socketHandler(socketHandler_),
      cryptoHandler(cryptoHandler_),
      socketFd(socketFd_),
      sequenceNumber(0) {}

bool BackedReader::hasData() {
  if (socketFd < 0) {
    return false;
  }

  if (localBuffer.size() > 0) {
    return true;
  }

  return socketHandler->hasData(socketFd);
}

int BackedReader::read(string* buf) {
  if (socketFd < 0) {
    // The socket is dead, return 0 bytes until it returns
    VLOG(1) << "Tried to read from a dead socket";

    // Sleep for a second
    sleep(1);
    return 0;
  }

  if (localBuffer.size() > 0) {
    VLOG(1) << "Reading from local buffer";
    string s = cryptoHandler->decrypt(localBuffer.front());
    localBuffer.pop_front();
    VLOG(1) << "New local buffer size: " << localBuffer.size();
    *buf = s;
    return 1;
  }

  // Read from the socket
  if (partialMessage.length() < 4) {
    // Read the header
    char tmpBuf[4];
    ssize_t bytesRead =
        socketHandler->read(socketFd, tmpBuf, 4 - partialMessage.length());
    if (bytesRead == 0) {
      // Connection is closed.  Instead of closing the socket, set EPIPE.
      // In EternalTCP, the server needs to explictly tell the client that
      // the session is over.
      errno = EPIPE;
      bytesRead = -1;
    }
    if (bytesRead > 0) {
      partialMessage.append(tmpBuf, bytesRead);
    }
    if (bytesRead < 0) {
      return bytesRead;
    }
  }
  if (partialMessage.length() < 4) {
    // We didn't get the full header yet.
    return 0;
  }
  int messageLength = getPartialMessageLength();
  VLOG(2) << "Reading message of length: " << messageLength;
  int messageRemainder = messageLength - (partialMessage.length() - 4);
  if (messageRemainder) {
    VLOG(2) << "bytes remaining: " << messageRemainder;
    string s(messageRemainder, '\0');
    ssize_t bytesRead = socketHandler->read(socketFd, &s[0], s.length());
    if (bytesRead == 0) {
      // Connection is closed.  Instead of closing the socket, set EPIPE.
      // In EternalTCP, the server needs to explictly tell the client that
      // the session is over.
      errno = EPIPE;
      bytesRead = -1;
    }
    if (bytesRead < 0) {
      VLOG(2) << "Error while reading";
      return bytesRead;
    } else if (bytesRead > 0) {
      partialMessage.append(&s[0], bytesRead);
      messageRemainder -= bytesRead;
    }
  }
  if (!messageRemainder) {
    constructPartialMessage(buf);
    return 1;
  }

  return 0;
}

void BackedReader::revive(int newSocketFd, vector<string> localBuffer_) {
  partialMessage = "";
  localBuffer.insert(localBuffer.end(), localBuffer_.begin(),
                     localBuffer_.end());
  sequenceNumber += localBuffer_.size();
  socketFd = newSocketFd;
}

int BackedReader::getPartialMessageLength() {
  if (partialMessage.length() < 4) {
    LOG(FATAL) << "Tried to construct a message header that wasn't complete";
  }
  int messageSize;
  memcpy(&messageSize, &partialMessage[0], sizeof(int));
  messageSize = ntohl(messageSize);
  return messageSize;
}

void BackedReader::constructPartialMessage(string* buf) {
  int messageSize = getPartialMessageLength();
  if (int(partialMessage.length()) - 4 < messageSize) {
    LOG(FATAL) << "Tried to construct a message that wasn't complete";
  }
  *buf = cryptoHandler->decrypt(partialMessage.substr(4, messageSize));
  partialMessage = partialMessage.substr(4 + messageSize);
  sequenceNumber++;
}
}  // namespace et
