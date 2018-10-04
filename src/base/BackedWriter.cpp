#include "BackedWriter.hpp"

namespace et {
BackedWriter::BackedWriter(std::shared_ptr<SocketHandler> socketHandler_,  //
                           std::shared_ptr<CryptoHandler> cryptoHandler_,  //
                           int socketFd_)
    : socketHandler(socketHandler_),
      cryptoHandler(cryptoHandler_),
      socketFd(socketFd_),
      backupSize(0),
      sequenceNumber(0) {}

BackedWriterWriteState BackedWriter::write(const string& buf) {
  // If recover started, Wait until finished
  lock_guard<std::mutex> guard(recoverMutex);
  string s = buf;
  {
    if (socketFd < 0) {
      // We have no socket to write to, don't bother trying to write
      LOG(INFO) << "SKIPPING WRITE";
      return BackedWriterWriteState::SKIPPED;
    }

    // Once we encrypt and the encryption state is updated, there's no
    // going back.
    s = cryptoHandler->encrypt(s);

    // Backup the buffer
    backupBuffer.push_front(s);
    backupSize += s.length();
    sequenceNumber++;
    // Cleanup old values
    while (backupSize > MAX_BACKUP_BYTES) {
      backupSize -= backupBuffer.back().length();
      backupBuffer.pop_back();
    }
  }

  // Size before we add the header
  int messageSize = s.length();

  messageSize = htonl(messageSize);
  s = string("0000") + s;
  memcpy(&s[0], &messageSize, sizeof(int));

  size_t bytesWritten = 0;
  size_t count = s.length();
  VLOG(2) << "Message length with header: " << count;

  while (true) {
    // We have a socket, let's try to use it.
    if (socketFd < 0) {
      LOG(INFO) << "WROTE WITH FAILURE";
      return BackedWriterWriteState::WROTE_WITH_FAILURE;
    }
    ssize_t result = socketHandler->write(
        socketFd, ((char*)&s[0]) + bytesWritten, count - bytesWritten);
    if (result >= 0) {
      bytesWritten += result;
      if (bytesWritten == count) {
        LOG(INFO) << "WROTE SUCCESS";
        return BackedWriterWriteState::SUCCESS;
      }
    } else if(errno == EAGAIN) {
      // Keep trying after 10ms
      ::usleep(10 * 1000);
    } else {
      // Error, we don't know how many bytes were written but it
      // doesn't matter because the reader is going to have to
      // reconnect anyways.  The important thing is for the caller to
      // think that the bytes were written and not call again.
      LOG(INFO) << "WROTE WITH FAILURE";
      return BackedWriterWriteState::WROTE_WITH_FAILURE;
    }
  }
}

vector<std::string> BackedWriter::recover(int64_t lastValidSequenceNumber) {
  if (socketFd >= 0) {
    throw std::runtime_error("Can't recover when the fd is still alive");
  }
  VLOG(1) << int64_t(this) << ": Manually locking recover mutex!";

  int64_t messagesToRecover = sequenceNumber - lastValidSequenceNumber;
  if (messagesToRecover < 0) {
    LOG(FATAL) << "Something went really wrong, client is ahead of server";
  }
  if (messagesToRecover == 0) {
    return vector<std::string>();
  }
  VLOG(1) << int64_t(this) << ": Recovering " << messagesToRecover
          << " Messages";
  int64_t messagesSeen = 0;
  vector<string> retval;
  for (auto it = backupBuffer.begin(); it != backupBuffer.end(); ++it) {
    retval.push_back(*it);
    messagesSeen++;
    if (messagesSeen == messagesToRecover) {
      reverse(retval.begin(), retval.end());
      return retval;
    }
  }
  throw new std::runtime_error("Client is too far behind server.");
}

void BackedWriter::revive(int newSocketFd) { socketFd = newSocketFd; }
}  // namespace et
