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
