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

  if (localBuffer.length() > 0) {
    return true;
  }

  return socketHandler->hasData(socketFd);
}

ssize_t BackedReader::read(void* buf, size_t count) {
  if (socketFd < 0) {
    // The socket is dead, return 0 bytes until it returns
    VLOG(1) << "Tried to read from a dead socket";

    // Sleep for a second
    sleep(1);
    return 0;
  }

  if (localBuffer.length() > 0) {
    VLOG(1) << "Reading from local buffer";
    // Read whatever we can from our local buffer and return
    size_t bytesToCopy = std::min(count, localBuffer.length());
    string s = string(&localBuffer[0], bytesToCopy);
    s = cryptoHandler->decrypt(s);
    memcpy(buf, &s[0], bytesToCopy);
    localBuffer = localBuffer.substr(bytesToCopy);  // TODO: Optimize
    VLOG(1) << "New local buffer size: " << localBuffer.length();
    return bytesToCopy;
  }

  // Read from the socket
  ssize_t bytesRead = socketHandler->read(socketFd, buf, count);
  if (bytesRead > 0) {
    sequenceNumber += bytesRead;
    string s((char*)buf, bytesRead);
    s = cryptoHandler->decrypt(s);
    memcpy(buf, &s[0], bytesRead);
  }
  return bytesRead;
}

void BackedReader::revive(int newSocketFd, std::string localBuffer_) {
  localBuffer.append(localBuffer_);
  sequenceNumber += localBuffer.length();
  socketFd = newSocketFd;
}
}
