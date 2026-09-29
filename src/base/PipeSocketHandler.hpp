#ifndef __ET_PIPE_SOCKET_HANDLER__
#define __ET_PIPE_SOCKET_HANDLER__

#include "UnixSocketHandler.hpp"

namespace et {
/**
 * @brief Handles UNIX domain socket connections that are represented as named
 * pipes.
 *
 * The connect/listen flow is shared; the platform-specific steps (socket
 * options, Windows' lack of client autobind, privilege-dropped variants) live
 * in PipeSocketHandlerUnix.cpp and PipeSocketHandlerWindows.cpp.
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
   * @brief Connects to a UNIX socket after dropping to @p uid/@p gid.
   *
   * Windows has no uid/gid privilege model, so this is a plain connect there.
   */
  int connectAsUser(const SocketEndpoint& endpoint, uid_t uid, gid_t gid);
  /**
   * @brief Creates a listening UNIX socket and stores it internally.
   */
  virtual set<int> listen(const SocketEndpoint& endpoint);
  /**
   * @brief Creates a listening UNIX socket after dropping to @p uid/@p gid.
   *
   * Windows has no uid/gid privilege model, so this is a plain listen there.
   */
  set<int> listenAsUser(const SocketEndpoint& endpoint, uid_t uid, gid_t gid);
  /**
   * @brief Returns the listening fds for a previously registered pipe.
   */
  virtual set<int> getEndpointFds(const SocketEndpoint& endpoint);
  /**
   * @brief Stops listening on the specified pipe and closes its fd.
   */
  virtual void stopListening(const SocketEndpoint& endpoint);
  /** @brief Closes a connection and removes its client socket path, if any. */
  void close(int fd) override;

  virtual void minimizeKernelBuffering(int fd);

 protected:
  /**
   * @brief Readies a fresh socket for connect().
   *
   * @param clientPath Receives a pathname the socket was bound to, which is
   *   removed when the socket closes. Left empty where the OS autobinds.
   * @return false if the socket cannot be used; the caller closes it.
   */
  bool prepareClientSocket(int fd, string* clientPath);
  /** @brief Configures a listening socket before bind(). */
  void prepareListenSocket(int fd);
  /** @brief Configures a listening socket after bind() and listen(). */
  void finishListenSocket(int fd, const string& pipePath);
  /** @brief Closes a client socket that never finished connecting. */
  void discardClientSocket(int fd, const string& clientPath);

  /** @brief Tracks path -> listening socket descriptors for each pipe. */
  map<string, set<int>> pipeServerSockets;
  /** @brief Client pathnames bound by prepareClientSocket, keyed by fd. */
  map<int, string> clientSocketPaths;
};
}  // namespace et

#endif  // __ET_TCP_SOCKET_HANDLER__
