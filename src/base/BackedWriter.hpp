#ifndef __ET_BACKED_WRITER__
#define __ET_BACKED_WRITER__

#include "CryptoHandler.hpp"
#include "Headers.hpp"
#include "Packet.hpp"
#include "SocketHandler.hpp"

namespace et {
enum class BackedWriterWriteState {
  SKIPPED = 0,  //
  SUCCESS = 1,
  WROTE_WITH_FAILURE = 2
};

class BackedWriter {
 public:
  BackedWriter(shared_ptr<SocketHandler> socketHandler,
               shared_ptr<CryptoHandler> cryptoHandler, int socketFd);

  BackedWriterWriteState write(Packet packet);

  vector<std::string> recover(int64_t lastValidSequenceNumber);

  void revive(int newSocketFd);
  mutex& getRecoverMutex() { return recoverMutex; }

  inline int getSocketFd() { return socketFd; }

  inline void invalidateSocket() {
    lock_guard<std::mutex> guard(recoverMutex);
    socketFd = -1;
  }

  inline int64_t getSequenceNumber() { return sequenceNumber; }

 protected:
  mutex recoverMutex;
  shared_ptr<SocketHandler> socketHandler;
  shared_ptr<CryptoHandler> cryptoHandler;
  int socketFd;

  static const int MAX_BACKUP_BYTES = 64 * 1024 * 1024;
  std::deque<Packet> backupBuffer;
  int64_t backupSize;
  int64_t sequenceNumber;
};
}  // namespace et

#endif  // __ET_BACKED_WRITER__
