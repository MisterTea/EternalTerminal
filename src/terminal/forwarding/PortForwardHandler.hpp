#ifndef __PORT_FORWARD_HANDLER_H__
#define __PORT_FORWARD_HANDLER_H__

#include "Connection.hpp"
#include "ETerminal.pb.h"
#include "ForwardDestinationHandler.hpp"
#include "ForwardSourceHandler.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Coordinates port forwarding requests, source/destination sockets, and
 * data flow.
 */
class PortForwardHandler {
 public:
  /**
   * @brief Constructs forwarding helpers for network and router sockets.
   * @param userid Session uid used for privilege-dropped UNIX socket ops
   *        ((uid_t)-1 to disable).
   * @param groupid Session gid used with @p userid.
   */
  explicit PortForwardHandler(shared_ptr<SocketHandler> _networkSocketHandler,
                              shared_ptr<SocketHandler> _pipeSocketHandler,
                              uid_t userid = static_cast<uid_t>(-1),
                              gid_t groupid = static_cast<gid_t>(-1));
  ~PortForwardHandler();

  PortForwardHandler(const PortForwardHandler&) = delete;
  PortForwardHandler& operator=(const PortForwardHandler&) = delete;

  /** @brief Polls handlers and collects destination requests and
   * `PortForwardData` for the caller to send. Touches only the descriptors
   * named in `readyFds`; `nullptr` polls every one. */
  void update(vector<PortForwardDestinationRequest>* requests,
              vector<PortForwardData>* dataToSend,
              const set<int>* readyFds = nullptr);
  /** @brief Handles control packets arriving over the SSH connection. */
  void handlePacket(const Packet& packet, shared_ptr<Connection> connection);
  PortForwardSourceResponse createSource(const PortForwardSourceRequest& pfsr,
                                         string* sourceName, uid_t userid,
                                         gid_t groupid);
  /** @brief Creates a remote destination handler that forwards data to a user's
   * socket. */
  PortForwardDestinationResponse createDestination(
      const PortForwardDestinationRequest& pfdr);

  /** @brief Tears down the source socket associated with `fd`. */
  void closeSourceFd(int fd);
  /** @brief Tracks a new source socket using the provided logical identifier.
   */
  void addSourceSocketId(int socketId, int sourceFd);
  /** @brief Tears down the source socket tied to the socket ID. */
  void closeSourceSocketId(int socketId);
  /** @brief Sends data back to the listener that originally accepted the source
   * socket. */
  void sendDataToSourceOnSocket(int socketId, const string& data);
  void getForwardFds(set<int>* fds);
  uint64_t getForwardFdsGeneration() const { return forwardFdsGeneration; }

 protected:
  /** @brief Handler used for the SSH/network-facing sockets. */
  shared_ptr<SocketHandler> networkSocketHandler;
  /** @brief Handler used for the router/pipe-facing sockets. */
  shared_ptr<SocketHandler> pipeSocketHandler;
  /** @brief Session uid for UNIX connect/listen; (uid_t)-1 disables drop. */
  uid_t sessionUid;
  /** @brief Session gid for UNIX connect/listen; (gid_t)-1 disables drop. */
  gid_t sessionGid;
  /** @brief Active destination handlers keyed by socket id. */
  unordered_map<int, unique_ptr<ForwardDestinationHandler>> destinationHandlers;

  /** @brief Handlers for the listening port forward sources. */
  vector<shared_ptr<ForwardSourceHandler>> sourceHandlers;
  /** @brief Maps control socket IDs to their source handlers for routing data.
   */
  unordered_map<int, shared_ptr<ForwardSourceHandler>> socketIdSourceHandlerMap;
  /** @brief Bumped by every site that opens or closes a descriptor exposed by
   * `getForwardFds`: a recycled fd number yields an identical set, so this is
   * a poller's only signal that it must re-register. */
  uint64_t forwardFdsGeneration = 0;
  vector<string> temporaryDirectories;
};
}  // namespace et

#endif  // __PORT_FORWARD_HANDLER_H__
