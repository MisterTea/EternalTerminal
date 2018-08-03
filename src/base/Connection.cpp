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

inline bool isSkippableError(int err_no) {
  return (err_no == ECONNRESET || err_no == ETIMEDOUT || err_no == EWOULDBLOCK ||
          err_no == EHOSTUNREACH || err_no == EPIPE ||
          err_no == EBADF  // Bad file descriptor can happen when
                          // there's a race condition between ta thread
                          // closing a connection and one
                          // reading/writing.
  );
}

ssize_t Connection::read(string* buf) {
  VLOG(4) << "before connectionMutex read";
  lock_guard<std::recursive_mutex> guard(connectionMutex);
  VLOG(4) << "get connectionMutex read";
  // 2s should be enough since EAGAIN is rare in blocking socket.
  int CLIENT_timeout = 2;
  // Try at 10Hz
  int num_trails = CLIENT_timeout * 10;
  for (int trials = 0; trials < num_trails; trials++) {
    ssize_t messagesRead = reader->read(buf);
    if (messagesRead == -1) {
      if (trials < (num_trails - 1) && errno == EAGAIN) {
        // If we get EAGAIN, assume the kernel needs to finish
        // flushing some buffer and retry after a delay.
        usleep(100000);
        LOG(INFO) << "Got EAGAIN, waiting 100ms...";
      } else if (trials == (num_trails - 1) && errno == EAGAIN) {
        // EAGAIN could possibly because a false alarm from hasData
        // before reconnect.
        // To give it a second chance, use a special signal to break out
        // of the loop.
        LOG(INFO) << "Got too much EAGAIN, assume there's nothing to read";
        return 2;
      } else if (isSkippableError(errno)) {
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
  exit(1);
}

bool Connection::readMessage(string* buf) {
  while (!shuttingDown) {
    ssize_t messagesRead = read(buf);
    if (messagesRead > 2 || messagesRead < -1) {
      LOG(FATAL) << "Invalid value for read(...) " << messagesRead;
    }
    if (messagesRead == 2) {
      LOG(INFO) << "Get EAGAIN signal, breaking out of read loop";
      break;
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
    LOG_EVERY_N(100, INFO) << "Read " << messagesRead
                           << " of message.  Waiting to read remainder...";
  }
  return false;
}

ssize_t Connection::write(const string& buf) {
  VLOG(4) << "before connectionMutex write";
  lock_guard<std::recursive_mutex> guard(connectionMutex);
  VLOG(4) << "get connectionMutex write";
  if (socketFd == -1) {
    return 0;
  }

  BackedWriterWriteState bwws = writer->write(buf);

  if (bwws == BackedWriterWriteState::SKIPPED) {
    VLOG(4) << "skipped";
    return 0;
  }

  if (bwws == BackedWriterWriteState::WROTE_WITH_FAILURE) {
    VLOG(4) << "wrote with failure";
    // Error writing.
    if (!errno) {
      // The socket was already closed
      VLOG(1) << "Socket closed";
    } else if (isSkippableError(errno)) {
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
    LOG_EVERY_N(1, INFO) << "Wrote " << bytesWritten
                           << " of message.  Waiting to write remainder...";
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
  VLOG(1) << "Closed socket";
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
