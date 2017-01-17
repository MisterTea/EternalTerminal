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

  virtual ssize_t read ( void* buf, size_t count );
  virtual ssize_t readAll ( void* buf, size_t count );

  virtual ssize_t write ( const void* buf, size_t count );
  virtual void writeAll ( const void* buf, size_t count );

  template<typename T> inline T readProto() {
    T t;
    int64_t length;
    readAll(&length, sizeof(int64_t));
    string s(length, 0);
    readAll(&s[0], length);
    t.ParseFromString(s);
    return t;
  }

  template<typename T> inline void writeProto(const T& t) {
    string s;
    t.SerializeToString(&s);
    int64_t length = s.length();
    writeAll(&length, sizeof(int64_t));
    writeAll(&s[0], length);
  }

  inline shared_ptr< BackedReader > getReader ( ) { return reader; }
  inline shared_ptr< BackedWriter > getWriter ( ) { return writer; }

  int getSocketFd ( ) { return socketFd; }

  int getClientId ( ) { return clientId; }

  inline bool hasData ( ) { return reader->hasData ( ); }

  virtual void closeSocket ( );

  void shutdown();

protected:
  bool recover ( int newSocketFd );

  shared_ptr< SocketHandler > socketHandler;
  string key;
  std::shared_ptr< BackedReader > reader;
  std::shared_ptr< BackedWriter > writer;
  int socketFd;
  int clientId;
  bool shuttingDown;
};
}

#endif  // __ETERNAL_TCP_CONNECTION__
