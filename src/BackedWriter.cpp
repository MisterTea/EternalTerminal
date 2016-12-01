#include "BackedWriter.hpp"

BackedWriter::BackedWriter(
  std::shared_ptr<SocketHandler> socketHandler_,
  int socketFd_) :
  socketHandler(socketHandler_),
  socketFd(socketFd_),
  immediateBackup(1024),
  sequenceNumber(0) {
}

void BackedWriter::backupBuffer(const void* buf, size_t count) {
  if (immediateBackup.size() > 0 &&
      immediateBackup.back().length()+count < BUFFER_CHUNK_SIZE) {
    // Append to the most recent element
    immediateBackup.back().append((const char*)buf,count);
  } else {
    // Create a new element.
    std::string s((const char *)buf, count);
    immediateBackup.push_back(s);
  }
}

ssize_t BackedWriter::write(const void* buf, size_t count) {
  // If recover started, Wait until finished
  std::lock_guard<std::mutex> guard(recoverMutex);
  if(socketFd<0) {
    // We have no socket to write to, write to the backup buffer and
    // exit
    backupBuffer(buf,count);
    return count;
  }

  // We have a socket, let's try to use it.
  backupBuffer(buf,count);
  sequenceNumber += count;
  ssize_t result = socketHandler->write(socketFd, buf, count);
  return result;
}

// TODO: We need to make sure no more data is written after recover is called
std::string BackedWriter::recover(int64_t lastValidSequenceNumber) {
  if (socketFd >= 0) {
    throw std::runtime_error("Can't recover when the fd is still alive");
  }
  VLOG(1) << int64_t(this) << ": Manually locking recover mutex!" << endl;
  recoverMutex.lock(); // Mutex is locked until we call revive

  int64_t bytesToRecover = sequenceNumber - lastValidSequenceNumber;
  if (bytesToRecover<0) {
    throw std::runtime_error("Something went really wrong, client is ahead of server");
  }
  if (bytesToRecover==0) {
    return "";
  }
  int64_t bytesSeen=0;
  std::string s;
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
      s.append(it->c_str() + (it->length() - bytesToWrite), bytesToWrite);
      for (auto it2 = it.base();
           it2 != immediateBackup.end();
           it2++) {
        s.append(it->c_str(), it->length());
      }
      return s;
    }
  }
  throw new std::runtime_error("Client is too far behind server.");
}

void BackedWriter::revive(int newSocketFd) {
  socketFd = newSocketFd;
}

void BackedWriter::unlock() {
  VLOG(1) << int64_t(this) << ": Manually unlocking recover mutex!" << endl;
  recoverMutex.unlock();
}
