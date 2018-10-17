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
  lock_guard<std::mutex> guard(recoverMutex);
  if (socketFd < 0) {
    return false;
  }

  if (localBuffer.size() > 0) {
    return true;
  }

  return socketHandler->hasData(socketFd);
}

int BackedReader::read(Packet* packet) {
  lock_guard<std::mutex> guard(recoverMutex);
  if (socketFd < 0) {
    // The socket is dead, return 0 bytes until it returns
    VLOG(1) << "Tried to read from a dead socket";
    return 0;
  }

  if (localBuffer.size() > 0) {
    VLOG(1) << "Reading from local buffer";
    *packet = Packet(localBuffer.front());
    localBuffer.pop_front();
    VLOG(1) << "New local buffer size: " << localBuffer.size();
    packet->decrypt(cryptoHandler);
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
      return -1;
    } else if (bytesRead > 0) {
      partialMessage.append(tmpBuf, bytesRead);
    } else if (bytesRead == -1 && errno == EAGAIN) {
      // We didn't get the full header yet.
      return 0;
    } else if (bytesRead == -1) {
      // Read error
      return -1;
    } else {
      LOG(FATAL) << "Read returned value outside of [-1,inf): " << bytesRead;
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
      return -1;
    } else if (bytesRead == -1) {
      VLOG(2) << "Error while reading";
      return bytesRead;
    } else if (bytesRead > 0) {
      partialMessage.append(&s[0], bytesRead);
      messageRemainder -= bytesRead;
    } else {
      LOG(FATAL) << "Invalid value from read: " << bytesRead;
    }
  }
  if (!messageRemainder) {
    constructPartialMessage(packet);
    return 1;
  }

  return 0;
}

void BackedReader::revive(int newSocketFd,
                          const vector<string>& newLocalEntries) {
  partialMessage = "";
  localBuffer.insert(localBuffer.end(), newLocalEntries.begin(),
                     newLocalEntries.end());
  sequenceNumber += newLocalEntries.size();
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

void BackedReader::constructPartialMessage(Packet* packet) {
  int messageSize = getPartialMessageLength();
  if (int(partialMessage.length()) - 4 != messageSize) {
    LOG(FATAL)
        << "Tried to construct a message that wasn't complete or over-filled: "
        << (int(partialMessage.length()) - 4) << " != " << messageSize;
  }
  string serializedPacket = partialMessage.substr(4, messageSize);
  *packet = Packet(serializedPacket);
  packet->decrypt(cryptoHandler);
  partialMessage.clear();
  sequenceNumber++;
}
}  // namespace et
