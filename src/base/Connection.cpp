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
  return (err_no == EAGAIN || err_no == ECONNRESET || err_no == ETIMEDOUT ||
          err_no == EWOULDBLOCK || err_no == EHOSTUNREACH || err_no == EPIPE ||
          err_no == EBADF  // Bad file descriptor can happen when
                           // there's a race condition between a thread
                           // closing a connection and one
                           // reading/writing.
  );
}

bool Connection::readMessage(Packet* packet) {
  while (!shuttingDown) {
    bool result = read(packet);
    if (result) {
      return true;
    }
    // Yield the processor
    usleep(100 * 1000);
    LOG_EVERY_N(10, INFO) << "Waiting to read...";
  }
  return false;
}

void Connection::writeMessage(const Packet& packet) {
  while (!shuttingDown) {
    bool success = write(packet);
    if (success) {
      return;
    }
    usleep(10 * 1000);
    LOG_EVERY_N(100, INFO) << "Waiting to write...";
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
  int fd = socketFd;
  socketFd = -1;
  socketHandler->close(fd);
  VLOG(1) << "Closed socket";
}

bool Connection::recover(int newSocketFd) {
  LOG(INFO) << "Locking reader/writer to recover...";
  lock_guard<std::mutex> readerGuard(reader->getRecoverMutex());
  lock_guard<std::mutex> writerGuard(writer->getRecoverMutex());
  LOG(INFO) << "Recovering with socket fd " << newSocketFd << "...";
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
    LOG(INFO) << "Finished recovering with socket fd: " << socketFd;
    return true;
  } catch (const runtime_error& err) {
    LOG(ERROR) << "Error recovering: " << err.what();
    socketHandler->close(newSocketFd);
    return false;
  }
}

void Connection::shutdown() {
  LOG(INFO) << "Shutting down connection";
  shuttingDown = true;
  Connection::closeSocket();
}

bool Connection::read(Packet* packet) {
  VLOG(4) << "Before read get connectionMutex";
  lock_guard<std::recursive_mutex> guard(connectionMutex);
  VLOG(4) << "After read get connectionMutex";
  ssize_t messagesRead = reader->read(packet);
  if (messagesRead == -1) {
    if (isSkippableError(errno)) {
      // Close the socket and invalidate, then return 0 messages
      LOG(INFO) << "Closing socket because " << errno << " " << strerror(errno);
      closeSocket();
      return 0;
    } else {
      // Throw the error
      LOG(ERROR) << "Got a serious error trying to read: " << errno << " / "
                 << strerror(errno);
      throw std::runtime_error("Failed a call to read");
    }
  } else {
    return messagesRead > 0;
  }
}

bool Connection::write(const Packet& packet) {
  lock_guard<std::recursive_mutex> guard(connectionMutex);
  if (socketFd == -1) {
    return false;
  }

  BackedWriterWriteState bwws = writer->write(packet);

  if (bwws == BackedWriterWriteState::SKIPPED) {
    VLOG(4) << "Write skipped";
    return false;
  }

  if (bwws == BackedWriterWriteState::WROTE_WITH_FAILURE) {
    VLOG(4) << "Wrote with failure";
    // Error writing.
    if (socketFd == -1) {
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
}  // namespace et
