#ifndef __ET_BACKED_READER__
#define __ET_BACKED_READER__

#include "Headers.hpp"

#include "CryptoHandler.hpp"
#include "Packet.hpp"
#include "SocketHandler.hpp"

namespace et {
class BackedReader {
 public:
  BackedReader(shared_ptr<SocketHandler> socketHandler,
               shared_ptr<CryptoHandler> cryptoHandler, int socketFd);

  bool hasData();
  int read(Packet* packet);

  mutex& getRecoverMutex() { return recoverMutex; }
  void revive(int newSocketFd, const vector<string>& newLocalEntries);

  inline void invalidateSocket() {
    lock_guard<std::mutex> guard(recoverMutex);
    socketFd = -1;
  }

  inline int64_t getSequenceNumber() { return sequenceNumber; }

 protected:
  mutex recoverMutex;
  shared_ptr<SocketHandler> socketHandler;
  shared_ptr<CryptoHandler> cryptoHandler;
  volatile int socketFd;
  int64_t sequenceNumber;
  deque<string> localBuffer;

  string partialMessage;

  void init(int64_t firstSequenceNumber);
  int getPartialMessageLength();
  void constructPartialMessage(Packet* packet);
};
}  // namespace et

#endif  // __ET_BACKED_READER__
