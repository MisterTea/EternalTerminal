#ifndef __ETERNAL_TCP_BACKED_READER__
#define __ETERNAL_TCP_BACKED_READER__

#include "Headers.hpp"

#include "SocketHandler.hpp"

class BackedReader {
public:
  explicit BackedReader(
    std::shared_ptr<SocketHandler> socketHandler,
    int socketFd);

  ssize_t read(void* buf, size_t count);

  void revive(int newSocketFd, std::string localBuffer_);

  inline void invalidateSocket() {
    socketFd = -1;
  }

  inline int64_t getSequenceNumber() {
    return sequenceNumber;
  }
protected:
  std::mutex recoverMutex;
  std::shared_ptr<SocketHandler> socketHandler;
  volatile int socketFd;
  int64_t sequenceNumber;
  std::string localBuffer;

  void init(int64_t firstSequenceNumber);
};

#endif // __ETERNAL_TCP_BACKED_READER__
