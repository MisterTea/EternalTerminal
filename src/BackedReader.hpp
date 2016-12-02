#ifndef __ETERNAL_TCP_BACKED_READER__
#define __ETERNAL_TCP_BACKED_READER__

#include "Headers.hpp"

#include "SocketHandler.hpp"
#include "CryptoHandler.hpp"

class BackedReader {
public:
  explicit BackedReader(
    shared_ptr<SocketHandler> socketHandler,
    shared_ptr<CryptoHandler> cryptoHandler,
    int socketFd);

  bool hasData();
  ssize_t read(void* buf, size_t count);

  void revive(int newSocketFd, std::string localBuffer_);

  inline void invalidateSocket() {
    // TODO: Close the socket
    socketFd = -1;
  }

  inline int64_t getSequenceNumber() {
    return sequenceNumber;
  }
protected:
  mutex recoverMutex;
  shared_ptr<SocketHandler> socketHandler;
  shared_ptr<CryptoHandler> cryptoHandler;
  volatile int socketFd;
  int64_t sequenceNumber;
  std::string localBuffer;

  void init(int64_t firstSequenceNumber);
};

#endif // __ETERNAL_TCP_BACKED_READER__
