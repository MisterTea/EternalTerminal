#include "Connection.hpp"

namespace et {
Connection::Connection(std::shared_ptr<SocketHandler> _socketHandler,
                       const string& _key)
    : socketHandler(_socketHandler), key(_key), shuttingDown(false) {}

Connection::~Connection() {
  if (!shuttingDown) {
    LOG(ERROR) << "Call shutdown before destructing a Connection.";
  }
  closeSocket();
}

inline bool isSkippableError() {
  return (errno == ECONNRESET || errno == ETIMEDOUT || errno == EAGAIN ||
          errno == EWOULDBLOCK || errno == EHOSTUNREACH);
}

ssize_t Connection::read(void* buf, size_t count) {
  ssize_t bytesRead = reader->read(buf, count);
  if (bytesRead == -1) {
    if (isSkippableError()) {
      // The connection has reset, close the socket and invalidate, then
      // return 0 bytes
      LOG(INFO) << "Closing socket because " << errno << " " << strerror(errno);
      closeSocket();
      bytesRead = 0;
    }
  }
  return bytesRead;
}

ssize_t Connection::readAll(void* buf, size_t count) {
  size_t pos = 0;
  while (pos < count && !shuttingDown) {
    ssize_t bytesRead = read(((char*)buf) + pos, count - pos);
    if (bytesRead < 0) {
      VLOG(1) << "Failed a call to readAll: %s\n" << strerror(errno);
      throw std::runtime_error("Failed a call to readAll");
    }
    pos += bytesRead;
    if (pos < count) {
      // Yield the processor
      usleep(1000);
    }
  }
  return count;
}

ssize_t Connection::write(const void* buf, size_t count) {
  if (socketFd == -1) {
    return 0;
  }

  BackedWriterWriteState bwws = writer->write(buf, count);

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

  return count;
}

void Connection::writeAll(const void* buf, size_t count) {
  while (!shuttingDown) {
    ssize_t bytesWritten = write(buf, count);
    if (bytesWritten > 0 && bytesWritten != (ssize_t)count) {
      LOG(FATAL) << "Somehow wrote a partial stream.  This shouldn't happen";
    }
    if (bytesWritten) {
      return;
    }
    usleep(1000);
  }
}

void Connection::closeSocket() {
  if (socketFd == -1) {
    LOG(ERROR) << "Tried to close a non-existent socket";
    return;
  }
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
      socketHandler->writeProto(newSocketFd, sh);
    }

    // Read the remote sequence number
    et::SequenceHeader remoteHeader =
        socketHandler->readProto<et::SequenceHeader>(newSocketFd);

    {
      // Fetch the catchup bytes and send
      et::CatchupBuffer catchupBuffer;
      catchupBuffer.set_buffer(writer->recover(remoteHeader.sequencenumber()));
      socketHandler->writeProto(newSocketFd, catchupBuffer);
    }

    et::CatchupBuffer catchupBuffer =
        socketHandler->readProto<et::CatchupBuffer>(newSocketFd);

    socketFd = newSocketFd;
    reader->revive(socketFd, catchupBuffer.buffer());
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
  closeSocket();
}
}
