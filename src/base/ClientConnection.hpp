#ifndef __ET_CLIENT_CONNECTION__
#define __ET_CLIENT_CONNECTION__

#include "Connection.hpp"
#include "Headers.hpp"

namespace et {
extern const int NULL_CLIENT_ID;

/**
 * @brief Connection implementation used by clients that connect to a remote
 * server.
 *
 * Handles reconnect logic by spawning a dedicated thread that waits for the
 * server to become reachable again.
 */
class ClientConnection : public Connection {
 public:
  ClientConnection(std::shared_ptr<SocketHandler> _socketHandler,
                   const SocketEndpoint& _endpoint, const string& _id,
                   const string& _key);

  virtual ~ClientConnection();

  /**
   * @brief Attempts to establish and authenticate a connection to the server.
   * @return true when the connection handshake succeeded.
   */
  bool connect();

  /**
   * @brief Adopts a session an earlier process left behind on the server.
   *
   * The server keeps a session alive when its client goes away (it cannot tell
   * a crashed client from one that is about to reconnect), and it recognizes a
   * returning client purely by id and key. So a *new* process holding the same
   * credentials can take the session over: connect, expect RETURNING_CLIENT,
   * then run the ordinary recovery handshake in take-over mode. The caller must
   * skip the initial-payload exchange, which belongs to session setup and would
   * desync a session that is already running.
   *
   * @return true when the session was adopted. False means there was nothing to
   * adopt (unknown or expired credentials, or too much output produced while we
   * were away to replay), and the caller should fall back to a fresh session.
   */
  bool attach();

  /**
   * @brief Extends the base behavior to spawn a reconnect thread after closing.
   */
  virtual void closeSocketAndMaybeReconnect();

  /**
   * @brief Blocks until any running reconnect thread has finished.
   */
  void waitReconnect();

 protected:
  /**
   * @brief Background loop used to re-establish a connection when lost.
   */
  void pollReconnect();

  /** @brief Server endpoint we try to connect to. */
  SocketEndpoint remoteEndpoint;
  /** @brief Thread that keeps retrying the handshake after disconnects. */
  std::shared_ptr<std::thread> reconnectThread;
};
}  // namespace et

#endif  // __ET_SERVER_CONNECTION__
