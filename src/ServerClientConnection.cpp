#include "ServerClientConnection.hpp"

bool ServerClientConnection::hasData() {
  return reader->hasData();
}

ssize_t ServerClientConnection::read(void* buf, size_t count) {
  ssize_t bytesRead = reader->read(buf, count);
  if (bytesRead == -1 && errno == ECONNRESET) {
    // The connection has reset, close the socket and invalidate, then
    // return 0 bytes
    closeSocket();
    bytesRead = 0;
  }
  return bytesRead;
}

ssize_t ServerClientConnection::readAll(void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesRead = read(((char*)buf) + pos, count - pos);
    if (bytesRead < 0) {
      VLOG(1) << "Failed a call to readAll: %s\n" << strerror(errno);
      throw std::runtime_error("Failed a call to readAll");
    }
    pos += bytesRead;
  }
  return count;
}

ssize_t ServerClientConnection::write(const void* buf, size_t count) {
  ssize_t bytesWritten = writer->write(buf, count);
  if(bytesWritten == -1) {
    // Error writing.
    if (errno == EPIPE) {
      // The connection has been severed, handle and hide from the caller
      closeSocket();
      bytesWritten = 0;
    }
  }

  // Success or some other error.
  return bytesWritten;
}

ssize_t ServerClientConnection::writeAll(const void* buf, size_t count) {
  size_t pos=0;
  while (pos<count) {
    ssize_t bytesWritten = write(((const char*)buf) + pos, count - pos);
    if (bytesWritten < 0) {
      VLOG(1) << "Failed a call to writeAll: " << strerror(errno);
      throw std::runtime_error("Failed a call to writeAll");
    }
    pos += bytesWritten;
  }
  return count;
}

void ServerClientConnection::closeSocket() {
  if (socketFd == -1) {
    throw std::runtime_error("Tried to close a non-existent socket");
  }
  reader->invalidateSocket();
  writer->invalidateSocket();
  socketFd = -1;
  socketHandler->close(socketFd);
  VLOG(1) << "Closed socket\n";
}
