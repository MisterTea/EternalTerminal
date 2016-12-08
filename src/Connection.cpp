#include "Connection.hpp"

namespace et {
Connection::Connection ( std::shared_ptr< SocketHandler > _socketHandler, const string& _key )
  : socketHandler ( _socketHandler ), key ( _key ), shuttingDown(false) {}

Connection::~Connection ( ) {
  if (!shuttingDown) {
    LOG(ERROR) << "Call shutdown before destructing a Connection.";
  }
  closeSocket ( );
}

ssize_t Connection::read ( void* buf, size_t count ) {
  ssize_t bytesRead = reader->read ( buf, count );
  if ( bytesRead == -1 ) {
    if ( errno == ECONNRESET || errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK ) {
      // The connection has reset, close the socket and invalidate, then
      // return 0 bytes
      closeSocket ( );
      bytesRead = 0;
    }
  }
  return bytesRead;
}

ssize_t Connection::readAll ( void* buf, size_t count ) {
  size_t pos = 0;
  while ( pos < count && !shuttingDown ) {
    ssize_t bytesRead = read ( ( ( char* ) buf ) + pos, count - pos );
    if ( bytesRead < 0 ) {
      VLOG ( 1 ) << "Failed a call to readAll: %s\n" << strerror ( errno );
      throw std::runtime_error ( "Failed a call to readAll" );
    }
    pos += bytesRead;
    if ( pos < count ) {
      // Yield the processor
      sleep ( 0 );
    }
  }
  return count;
}

ssize_t Connection::write ( const void* buf, size_t count ) {
  BackedWriterWriteState bwws = writer->write ( buf, count );

  if ( bwws == BackedWriterWriteState::SKIPPED ) {
    VLOG(1) << "Write skipped";
    return 0;
  }

  if ( bwws == BackedWriterWriteState::WROTE_WITH_FAILURE ) {
    // Error writing.
    if ( !errno ) {
      // The socket was already closed
      VLOG(1) << "Socket closed";
    } else if ( errno == EPIPE || errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK ) {
      VLOG(1) << " Connection is severed";
      // The connection has been severed, handle and hide from the caller
      closeSocket ( );
    } else {
      LOG ( FATAL ) << "Unexpected socket error: " << errno << " " << strerror ( errno );
    }
  }

  return count;
}

void Connection::writeAll ( const void* buf, size_t count ) {
  while ( !shuttingDown ) {
    if ( write ( buf, count ) ) {
      return;
    }
    sleep ( 0 );
  }
}

void Connection::closeSocket ( ) {
  if ( socketFd == -1 ) {
    LOG ( ERROR ) << "Tried to close a non-existent socket";
    return;
  }
  reader->invalidateSocket ( );
  writer->invalidateSocket ( );
  socketHandler->close ( socketFd );
  socketFd = -1;
  VLOG ( 1 ) << "Closed socket\n";
}

bool Connection::recover ( int newSocketFd ) {
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
    return true;
  } catch ( const runtime_error& err ) {
    LOG ( ERROR ) << "Error recovering: " << err.what ( );
    socketHandler->close ( newSocketFd );
    writer->unlock ( );
    return false;
  }
}

  void Connection::shutdown() {
    LOG(INFO) << "Shutting down connection";
    shuttingDown = true;
    closeSocket();
  }
}
