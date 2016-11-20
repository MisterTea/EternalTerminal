#include "BackedReader.hpp"

BackedReader::BackedReader(
  std::shared_ptr<SocketHandler> socketHandler_,
  int socketFd_) :
  socketHandler(socketHandler_),
  socketFd(socketFd_),
  sequenceNumber(0) {
}

ssize_t BackedReader::read(void* buf, size_t count) {
  while (socketFd<0) {
    // The socket is dead, wait for it to return
    sleep(1);
  }

  if (localBuffer.length()>0) {
    // Read whatever we can from our local buffer and return
    size_t bytesToCopy = std::min(count, localBuffer.length());
    memcpy(buf, &localBuffer[0], bytesToCopy);
    localBuffer = localBuffer.substr(bytesToCopy); // TODO: Optimize
    return bytesToCopy;
  }

  // Read from the socket
  ssize_t bytesRead = socketHandler->read(socketFd, buf, count);
  sequenceNumber += bytesRead;
  return bytesRead;
}

void BackedReader::revive(int newSocketFd, std::string localBuffer_) {
  localBuffer = localBuffer_;
  socketFd = newSocketFd;
}
