#ifndef __ETERNAL_TCP_BACKED_READER__
#define __ETERNAL_TCP_BACKED_READER__

#include "Headers.hpp"

class BackedReader {
public:
  explicit BackedReader(int socket_fd);

  explicit BackedReader(
    int socket_fd,
    int client_id,
    int64_t firstSequenceNumber);

protected:
  int socket_fd;
  int client_id; //TODO: Change client_id to be a boost::uuids::uuid
  std::string recoverBuffer;
};

#endif // __ETERNAL_TCP_BACKED_READER__
