#ifndef __ETERNAL_TCP_BACKED_WRITER__
#define __ETERNAL_TCP_BACKED_WRITER__

#include "Headers.hpp"

#include "SocketHandler.hpp"

class BackedWriter {
public:
  explicit BackedWriter(
    std::shared_ptr<SocketHandler> socketHandler,
    int socket_fd);

  ssize_t write(const void* buf, size_t count);

  bool recover(int new_socket_fd, int64_t lastValidSequenceNumber);
protected:
  static const int BUFFER_CHUNK_SIZE = 64*1024;

  std::shared_ptr<SocketHandler> socketHandler;
  int socket_fd;

  // TODO: Change std::string -> std::array with length, this way it's preallocated
  boost::circular_buffer<std::string> immediateBackup;
  //std::vector<std::string> longTermBackup; // Not implemented yet
  int64_t sequenceNumber;
};

#endif // __ETERNAL_TCP_BACKED_WRITER__
