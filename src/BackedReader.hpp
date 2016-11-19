#ifndef __ETERNAL_TCP_BACKED_READER__
#define __ETERNAL_TCP_BACKED_READER__

#include "Headers.hpp"

#include "SocketHandler.hpp"

class BackedReader {
public:
  explicit BackedReader(
    std::shared_ptr<SocketHandler> socketHandler,
    int socket_fd);

  explicit BackedReader(
    std::shared_ptr<SocketHandler> socketHandler,
    int socket_fd,
    int client_id,
    int64_t firstSequenceNumber);

  ssize_t read(void* buf, size_t count);
protected:
  std::shared_ptr<SocketHandler> socketHandler;
  int socket_fd;
  int client_id; //TODO: Change client_id to be a boost::uuids::uuid
  std::string recoverBuffer;
};

#endif // __ETERNAL_TCP_BACKED_READER__
