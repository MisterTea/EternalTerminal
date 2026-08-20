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
   * @brief True when the last successful connect used the reset handshake
   * (the server already held state for this id, so no bootstrap exchange
   * is needed).
   */
  bool wasRecovered() const { return recovered_; }

  /**
   * @brief The ConnectStatus received on the most recent connect attempt.
   * Only meaningful when connect() returned false.
   */
  et::ConnectStatus lastStatus() const { return lastStatus_; }

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
  /** @brief Set when connect() completed via the reset handshake. */
  bool recovered_ = false;
  /** @brief ConnectStatus from the most recent connect attempt. */
  et::ConnectStatus lastStatus_ = et::ConnectStatus::NEW_CLIENT;
  /** @brief Thread that keeps retrying the handshake after disconnects. */
  std::shared_ptr<std::thread> reconnectThread;
};
}  // namespace et

#endif  // __ET_SERVER_CONNECTION__
