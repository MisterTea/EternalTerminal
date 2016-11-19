#include "BackedReader.hpp"

BackedReader::BackedReader(int socket_fd_) :
  socket_fd(socket_fd_),
  client_id(rand()) {
}

BackedReader::BackedReader(
  int socket_fd_,
  int client_id_,
  int64_t firstSequenceNumber) :
  socket_fd(socket_fd_),
  client_id(client_id_) {
  // Send client id & first sequence number
}
