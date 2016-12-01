#ifndef __ETERNAL_TCP_BACKED_WRITER__
#define __ETERNAL_TCP_BACKED_WRITER__

#include "Headers.hpp"

#include "SocketHandler.hpp"

class BackedWriter {
public:
  explicit BackedWriter(
    std::shared_ptr<SocketHandler> socketHandler,
    int socketFd);

  ssize_t write(const void* buf, size_t count);

  std::string recover(int64_t lastValidSequenceNumber);

  void revive(int newSocketFd);
  void unlock();

  inline int getSocketFd() {
    return socketFd;
  }

  inline void invalidateSocket() {
    // TODO: Close the socket
    socketFd = -1;
  }
protected:
  static const int BUFFER_CHUNK_SIZE = 64*1024;

  void backupBuffer(const void* buf, size_t count);

  std::mutex recoverMutex;
  std::shared_ptr<SocketHandler> socketHandler;
  int socketFd;

  // TODO: Change std::string -> std::array with length, this way it's preallocated
  boost::circular_buffer<std::string> immediateBackup;
  //std::vector<std::string> longTermBackup; // Not implemented yet
  int64_t sequenceNumber;
};

#endif // __ETERNAL_TCP_BACKED_WRITER__
