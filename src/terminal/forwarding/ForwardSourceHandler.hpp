#ifndef __FORWARD_SOURCE_HANDLER_H__
#define __FORWARD_SOURCE_HANDLER_H__

#include "Headers.hpp"
#include "SocketHandler.hpp"
#include "SocksUtils.hpp"

namespace et {
/**
 * @brief Accepts incoming connections on a local endpoint and tracks open
 * sockets.
 */
class ForwardSourceHandler {
 public:
  /**
   * @brief Creates source/destination handlers used for local port forwarding.
   * @param alreadyListening If true, skip listen(); caller already registered
   * the source endpoint (e.g. via listenAsUser).
   * @param socksDynamic If true, destination is chosen per-connection via a
   * SOCKS4/SOCKS5 handshake after accept (ssh -D).
   */
  ForwardSourceHandler(shared_ptr<SocketHandler> _socketHandler,
                       const SocketEndpoint& _source,
                       const SocketEndpoint& _destination,
                       bool alreadyListening = false,
                       bool socksDynamic = false);

  /**
   * @brief Bridges an already-open read/write pair (e.g. stdio for -W) to a
   * fixed remote destination without listening.
   * @param closeFds If false, destruction leaves the fds open (stdin/stdout).
   */
  ForwardSourceHandler(shared_ptr<SocketHandler> _socketHandler,
                       const SocketEndpoint& _destination, int readFd,
                       int writeFd, bool closeFds);

  ~ForwardSourceHandler();

  ForwardSourceHandler(const ForwardSourceHandler&) = delete;
  ForwardSourceHandler& operator=(const ForwardSourceHandler&) = delete;

  /**
   * @brief Accepts one pending connection (or completes one SOCKS/stdio
   * handshake) and returns its fd, or -1 if none. When non-null,
   * `destinationOut` receives the remote endpoint for this connection.
   * Accepts only on endpoints named in `readyFds`; `nullptr` tries every one.
   */
  int listen(SocketEndpoint* destinationOut = nullptr,
             const set<int>* readyFds = nullptr);

  /** @brief Reads the sockets named in `readyFds` (all of them when `nullptr`)
   * and stages `PortForwardData` for destinations.
   * @return true if any socket was closed and dropped. */
  bool update(vector<PortForwardData>* data,
              const set<int>* readyFds = nullptr);

  /** @brief Returns true if an accepted socket is pending assignment. */
  bool hasUnassignedFd(int fd);

  /** @brief Closes sockets that were accepted but not yet assigned an ID. */
  void closeUnassignedFd(int fd);

  /** @brief Maps a socketId (from the control channel) to a pending fd. */
  void addSocket(int socketId, int sourceFd);

  /**
   * @brief Writes the SOCKS CONNECT reply for `fd` after the remote
   * destination accepts or fails. No-op when `fd` is not a SOCKS client.
   */
  void finishSocksConnect(int fd, bool success);

  /** @brief True while a `-W` bridge can still receive destination bytes. */
  bool stdioBridgeOpen() const;

  /** @brief Closes the socket mapped to `socketId`. */
  void closeSocket(int socketId);

  /** @brief Sends bytes from the remote side down the local source socket. */
  void sendDataOnSocket(int socketId, const string& data);

  void getActiveFds(set<int>* fds);

  inline SocketEndpoint getDestination() { return destination; }
  inline SocketEndpoint getSource() { return source; }

  /** @brief True when this handler bridges stdio (-W) rather than a listener.
   */
  bool isStdioForward() const { return stdioMode; }

 protected:
  int acceptFixed(const set<int>* readyFds);
  int takeCompletedSocks(SocketEndpoint* destinationOut,
                         const set<int>* readyFds);
  void advanceSocksHandshakes(const set<int>* readyFds);

  /** @brief Socket helper used to accept connections on the source endpoint. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief Local endpoint clients connect to for port forwarding. */
  SocketEndpoint source;
  /** @brief Remote destination endpoint that receives forwarded data. */
  SocketEndpoint destination;
  /** @brief When true, parse SOCKS to choose destination after accept. */
  bool socksDynamic = false;
  /** @brief When true, bridge a provided read/write fd pair (no listen). */
  bool stdioMode = false;
  /** @brief When false, do not close read/write fds in the destructor. */
  bool closeOwnedFds = true;
  /** @brief True until the stdio bridge has emitted its destination request. */
  bool stdioRequestPending = false;
  /** @brief Stdin hit EOF; stop reading but keep stdout until the remote
   * closes. */
  bool stdioReadClosed = false;
  int stdioReadFd = -1;
  int stdioWriteFd = -1;
  /** @brief Sockets that are awaiting assignment from the control stream. */
  unordered_set<int> unassignedFds;
  /** @brief Maps logical socket IDs to their accepted file descriptors. */
  unordered_map<int, int> socketFdMap;
  /** @brief Optional separate write fd when different from the read fd (-W). */
  unordered_map<int, int> socketWriteFdMap;
  /** @brief In-progress SOCKS handshakes keyed by accepted client fd. */
  unordered_map<int, SocksHandshake> socksPending;
  struct SocksAwaitingReply {
    int version = 0;
    string earlyData;
  };
  /** @brief Completed handshakes waiting for the destination response. */
  unordered_map<int, SocksAwaitingReply> socksAwaitingReply;
  /** @brief Application bytes that arrived with the CONNECT request. */
  unordered_map<int, string> socksEarlyPayload;
};
}  // namespace et

#endif  // __FORWARD_SOURCE_HANDLER_H__
