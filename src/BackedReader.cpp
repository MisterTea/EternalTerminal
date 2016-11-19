#include "BackedReader.hpp"

BackedReader::BackedReader(
  std::shared_ptr<SocketHandler> socketHandler_,
  int socket_fd_) :
  socketHandler(socketHandler_),
  socket_fd(socket_fd_),
  client_id(rand()) {
}

BackedReader::BackedReader(
  std::shared_ptr<SocketHandler> socketHandler_,
  int socket_fd_,
  int client_id_,
  int64_t firstSequenceNumber) :
  socketHandler(socketHandler_),
  socket_fd(socket_fd_),
  client_id(client_id_) {
  // Send client id & first sequence number
}

ssize_t BackedReader::read(void* buf, size_t count) {
  return socketHandler->read(socket_fd, buf, count);
}
