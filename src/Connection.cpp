#include "Connection.hpp"

namespace et {
Connection::Connection(shared_ptr<SocketHandler> _socketHandler,
                       const string& _id, const string& _key)
    : socketHandler(_socketHandler), id(_id), key(_key), shuttingDown(false) {}

Connection::~Connection() {
  if (!shuttingDown) {
    LOG(ERROR) << "Call shutdown before destructing a Connection.";
  }
  if (socketFd != -1) {
    LOG(INFO) << "Connection destroyed";
    Connection::closeSocket();
  }
}

inline bool isSkippableError() {
  return (errno == ECONNRESET || errno == ETIMEDOUT || errno == EWOULDBLOCK ||
          errno == EHOSTUNREACH || errno == EPIPE ||
          errno == EBADF  // Bad file descriptor can happen when
                          // there's a race condition between ta thread
                          // closing a connection and one
                          // reading/writing.
  );
}

ssize_t Connection::read(string* buf) {
  lock_guard<std::recursive_mutex> guard(connectionMutex);
  for (int trials = 0; trials < 20; trials++) {
    ssize_t messagesRead = reader->read(buf);
    if (messagesRead == -1) {
      if (trials < (20 - 1) && errno == EAGAIN) {
        // If we get EAGAIN, assume the kernel needs to finish
        // flushing some buffer and retry after a delay.
        usleep(100000);
      } else if (isSkippableError()) {
        // Close the socket and invalidate, then return 0 messages
        LOG(INFO) << "Closing socket because " << errno << " "
                  << strerror(errno);
        closeSocket();
        return 0;
      } else {
        // Pass the error up the stack.
        return -1;
      }
    } else {
      // Success
      return messagesRead;
    }
  }

  // Should never get here
  LOG(FATAL) << "Invalid trials iteration";
}

bool Connection::readMessage(string* buf) {
  while (!shuttingDown) {
    ssize_t messagesRead = read(buf);
    if (messagesRead > 1 || messagesRead < -1) {
      LOG(FATAL) << "Invalid value for read(...) " << messagesRead;
    }
    if (messagesRead == 1) {
      return true;
    }
    if (messagesRead == -1) {
      VLOG(1) << "Failed a call to readAll: %s\n" << strerror(errno);
      throw std::runtime_error("Failed a call to readAll");
    }
    // Yield the processor
    usleep(1000);
  }
  return false;
}

ssize_t Connection::write(const string& buf) {
  lock_guard<std::recursive_mutex> guard(connectionMutex);
  if (socketFd == -1) {
    return 0;
  }

  BackedWriterWriteState bwws = writer->write(buf);

  if (bwws == BackedWriterWriteState::SKIPPED) {
    return 0;
  }

  if (bwws == BackedWriterWriteState::WROTE_WITH_FAILURE) {
    // Error writing.
    if (!errno) {
      // The socket was already closed
      VLOG(1) << "Socket closed";
    } else if (isSkippableError()) {
      VLOG(1) << " Connection is severed";
      // The connection has been severed, handle and hide from the caller
      closeSocket();
    } else {
      LOG(FATAL) << "Unexpected socket error: " << errno << " "
                 << strerror(errno);
    }
  }

  return 1;
}

void Connection::writeMessage(const string& buf) {
  while (!shuttingDown) {
    ssize_t bytesWritten = write(buf);
    if (bytesWritten) {
      return;
    }
    usleep(1000);
  }
}

void Connection::closeSocket() {
  lock_guard<std::recursive_mutex> guard(connectionMutex);
  if (socketFd == -1) {
    LOG(ERROR) << "Tried to close a dead socket";
    return;
  }
  // TODO: There is a race condition where we invalidate and another
  // thread can try to read/write to the socket.  For now we handle the
  // error but it would be better to avoid it.
  reader->invalidateSocket();
  writer->invalidateSocket();
  socketHandler->close(socketFd);
  socketFd = -1;
  VLOG(1) << "Closed socket\n";
}

bool Connection::recover(int newSocketFd) {
  LOG(INFO) << "Recovering...";
  try {
    {
      // Write the current sequence number
      et::SequenceHeader sh;
      sh.set_sequencenumber(reader->getSequenceNumber());
      socketHandler->writeProto(newSocketFd, sh, true);
    }

    // Read the remote sequence number
    et::SequenceHeader remoteHeader =
        socketHandler->readProto<et::SequenceHeader>(newSocketFd, true);

    {
      // Fetch the catchup bytes and send
      et::CatchupBuffer catchupBuffer;
      vector<string> recoveredMessages =
          writer->recover(remoteHeader.sequencenumber());
      for (auto it : recoveredMessages) {
        catchupBuffer.add_buffer(it);
      }
      socketHandler->writeProto(newSocketFd, catchupBuffer, true);
    }

    et::CatchupBuffer catchupBuffer =
        socketHandler->readProto<et::CatchupBuffer>(newSocketFd, true);

    socketFd = newSocketFd;
    vector<string> recoveredMessages(catchupBuffer.buffer().begin(),
                                     catchupBuffer.buffer().end());
    reader->revive(socketFd, recoveredMessages);
    writer->revive(socketFd);
    writer->unlock();
    LOG(INFO) << "Finished recovering";
    return true;
  } catch (const runtime_error& err) {
    LOG(ERROR) << "Error recovering: " << err.what();
    socketHandler->close(newSocketFd);
    writer->unlock();
    return false;
  }
}

void Connection::shutdown() {
  LOG(INFO) << "Shutting down connection";
  shuttingDown = true;
  Connection::closeSocket();
}
}  // namespace et
