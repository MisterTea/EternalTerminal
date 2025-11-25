#ifndef __FORWARD_SOURCE_HANDLER_H__
#define __FORWARD_SOURCE_HANDLER_H__

#include "Headers.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Accepts incoming connections on a local endpoint and tracks open sockets.
 */
class ForwardSourceHandler {
 public:
  /** @brief Creates source/destination handlers used for local port forwarding. */
  ForwardSourceHandler(shared_ptr<SocketHandler> _socketHandler,
                       const SocketEndpoint& _source,
                       const SocketEndpoint& _destination);

  ~ForwardSourceHandler();

  /** @brief Starts listening on the source endpoint and returns the server fd. */
  int listen();

  /** @brief Polls all active sockets and stages `PortForwardData` for destinations. */
  void update(vector<PortForwardData>* data);

  /** @brief Returns true if an accepted socket is pending assignment. */
  bool hasUnassignedFd(int fd);

  /** @brief Closes sockets that were accepted but not yet assigned an ID. */
  void closeUnassignedFd(int fd);

  /** @brief Maps a socketId (from the control channel) to a pending fd. */
  void addSocket(int socketId, int sourceFd);

  /** @brief Closes the socket mapped to `socketId`. */
  void closeSocket(int socketId);

  /** @brief Sends bytes from the remote side down the local source socket. */
  void sendDataOnSocket(int socketId, const string& data);

  inline SocketEndpoint getDestination() { return destination; }

 protected:
  /** @brief Socket helper used to accept connections on the source endpoint. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief Local endpoint clients connect to for port forwarding. */
  SocketEndpoint source;
  /** @brief Remote destination endpoint that receives forwarded data. */
  SocketEndpoint destination;
  /** @brief Sockets that are awaiting assignment from the control stream. */
  unordered_set<int> unassignedFds;
  /** @brief Maps logical socket IDs to their accepted file descriptors. */
  unordered_map<int, int> socketFdMap;
};
}  // namespace et

#endif  // __FORWARD_SOURCE_HANDLER_H__
