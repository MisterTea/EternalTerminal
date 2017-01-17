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

  BackedWriterWriteState write(const void* buf, size_t count);

  std::string recover(int64_t lastValidSequenceNumber);

  void revive(int newSocketFd);
  void unlock();

  inline int getSocketFd() { return socketFd; }

  inline void invalidateSocket() {
    lock_guard<std::mutex> guard(recoverMutex);
    socketFd = -1;
  }

  inline int64_t getSequenceNumber() { return sequenceNumber; }

 protected:
  static const int BUFFER_CHUNK_SIZE = 64 * 1024;

  void backupBuffer(const void* buf, size_t count);

  mutex recoverMutex;
  shared_ptr<SocketHandler> socketHandler;
  shared_ptr<CryptoHandler> cryptoHandler;
  int socketFd;

  // TODO: Change std::string -> std::array with length, this way it's
  // preallocated
  boost::circular_buffer<std::string> immediateBackup;
  // std::vector<std::string> longTermBackup; // Not implemented yet
  int64_t sequenceNumber;
};
}

#endif  // __ETERNAL_TCP_BACKED_WRITER__
