#ifndef __ETERNAL_TCP_CONNECTION__
#define __ETERNAL_TCP_CONNECTION__

#include "Headers.hpp"

#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "SocketHandler.hpp"

namespace et {
class Connection {
 public:
  Connection ( std::shared_ptr< SocketHandler > _socketHandler, const string& key );

  virtual ~Connection ( );

  ssize_t read ( void* buf, size_t count );
  ssize_t readAll ( void* buf, size_t count );

  ssize_t write ( const void* buf, size_t count );
  void writeAll ( const void* buf, size_t count );

  inline shared_ptr< BackedReader > getReader ( ) { return reader; }
  inline shared_ptr< BackedWriter > getWriter ( ) { return writer; }

  int getSocketFd ( ) { return socketFd; }

  int getClientId ( ) { return clientId; }

  inline bool hasData ( ) { return reader->hasData ( ); }

 protected:
  virtual void closeSocket ( );
  bool recover ( int newSocketFd );

  shared_ptr< SocketHandler > socketHandler;
  string key;
  std::shared_ptr< BackedReader > reader;
  std::shared_ptr< BackedWriter > writer;
  int socketFd;
  int clientId;
};
}

#endif  // __ETERNAL_TCP_CONNECTION__
