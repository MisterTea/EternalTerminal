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
#ifndef WIN32
  /**
   * @brief Connects to a UNIX socket after dropping to @p uid/@p gid.
   */
  int connectAsUser(const SocketEndpoint& endpoint, uid_t uid, gid_t gid);
#endif
  /**
   * @brief Creates a listening UNIX socket and stores it internally.
   */
  virtual set<int> listen(const SocketEndpoint& endpoint);
#ifndef WIN32
  /**
   * @brief Creates a listening UNIX socket after dropping to @p uid/@p gid.
   */
  set<int> listenAsUser(const SocketEndpoint& endpoint, uid_t uid, gid_t gid);
#endif
  /**
   * @brief Returns the listening fds for a previously registered pipe.
   */
  virtual set<int> getEndpointFds(const SocketEndpoint& endpoint);
  /**
   * @brief Stops listening on the specified pipe and closes its fd.
   */
  virtual void stopListening(const SocketEndpoint& endpoint);
  /** @brief Closes a connection and removes its Windows client socket path. */
  void close(int fd) override;

  virtual void minimizeKernelBuffering(int fd);

 protected:
  /** @brief Tracks path -> listening socket descriptors for each pipe. */
  map<string, set<int>> pipeServerSockets;
#ifdef WIN32
  /** @brief Client pathname required because Windows AF_UNIX has no autobind.
   */
  map<int, string> clientSocketPaths;
#endif
};
}  // namespace et

#endif  // __ET_TCP_SOCKET_HANDLER__
