#ifndef __ET_PIPE_SOCKET_HANDLER__
#define __ET_PIPE_SOCKET_HANDLER__

#include "UnixSocketHandler.hpp"

namespace et {
/**
 * @brief Handles UNIX domain socket connections that are represented as named
 * pipes.
 */
class PipeSocketHandler : public UnixSocketHandler {
 public:
  PipeSocketHandler();
  virtual ~PipeSocketHandler() {}

  /**
   * @brief Connects to a pipe identified by the endpoint name.
   */
  virtual int connect(const SocketEndpoint& endpoint);
  /**
   * @brief Creates a listening UNIX socket and stores it internally.
   */
  virtual set<int> listen(const SocketEndpoint& endpoint);
  /**
   * @brief Returns the listening fds for a previously registered pipe.
   */
  virtual set<int> getEndpointFds(const SocketEndpoint& endpoint);
  /**
   * @brief Stops listening on the specified pipe and closes its fd.
   */
  virtual void stopListening(const SocketEndpoint& endpoint);

 protected:
  /** @brief Tracks path -> listening socket descriptors for each pipe. */
  map<string, set<int>> pipeServerSockets;
};
}  // namespace et

#endif  // __ET_TCP_SOCKET_HANDLER__
