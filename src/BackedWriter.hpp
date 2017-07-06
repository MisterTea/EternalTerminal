#ifndef __ETERNAL_TCP_BACKED_WRITER__
#define __ETERNAL_TCP_BACKED_WRITER__

#include "Headers.hpp"

#include "CryptoHandler.hpp"
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

  BackedWriterWriteState write(const string& buf);

  vector<std::string> recover(int64_t lastValidSequenceNumber);

  void revive(int newSocketFd);
  void unlock();

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
  std::deque<string> backupBuffer;
  int64_t backupSize;
  int64_t sequenceNumber;
};
}  // namespace et

#endif  // __ETERNAL_TCP_BACKED_WRITER__
