#include "BackedWriter.hpp"

BackedWriter::BackedWriter(
  std::shared_ptr<SocketHandler> socketHandler_,
  int socket_fd_) :
  socketHandler(socketHandler_),
  socket_fd(socket_fd_),
  immediateBackup(1024),
  sequenceNumber(0) {
}


ssize_t BackedWriter::write(const void* buf, size_t count) {
  if (immediateBackup.size() > 0 &&
      immediateBackup.back().length()+count < BUFFER_CHUNK_SIZE) {
    // Append to the most recent element
    immediateBackup.back().append((const char*)buf,count);
  } else {
    // Create a new element
    std::string s((const char *)buf, count);
    immediateBackup.push_back(s);
  }
  sequenceNumber += count;
  if (socket_fd>=0) {
    ssize_t result = socketHandler->write(socket_fd, buf, count);
    if(result != (ssize_t)count) {
      // Error writing.
      if (errno == EPIPE) {
        // The connection has been severed, handle and hide from the caller
        // TODO: Start backing up circular buffer to disk/vector
        socket_fd = -1;
        return count;
      } else {
        // Some other error, don't handle and
        return result;
      }
    } else {
      return count;
    }
  } else {
    // Pretend we fnished writing.
    return count;
  }
}

bool BackedWriter::recover(int new_socket_fd, int64_t lastValidSequenceNumber) {
  socket_fd = new_socket_fd;
  int64_t bytesToRecover = sequenceNumber - lastValidSequenceNumber;
  if (bytesToRecover<0) {
    fprintf(stderr, "Something went really wrong, client is ahead of server\n");
    fflush(stderr);
    exit(1);
  }
  if (bytesToRecover==0) {
    return true;
  }
  int64_t bytesSeen=0;
  for (
    auto it = immediateBackup.rbegin();
    it != immediateBackup.rend();
    ++it) {
    if (bytesSeen + (int64_t)it->length() < bytesToRecover) {
      // We need to keep going in the circular buffer
      bytesSeen += it->length();
      continue;
    } else {
      // Start recovering
      int64_t bytesToWrite = std::min((int64_t)it->length(),bytesToRecover);
      socketHandler->write(socket_fd, it->c_str() + (it->length() - bytesToWrite), bytesToWrite);
      for (auto it2 = it.base();
           it2 != immediateBackup.end();
           it2++) {
        socketHandler->write(socket_fd, it->c_str(), it->length());
      }
      return true;
    }
  }
  fprintf(stderr, "Client is too far behind server.");
  fflush(stderr);
  exit(1);
  socket_fd = -1;
  return false;
}
