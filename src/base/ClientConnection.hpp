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
                   const string& _key, bool _resetIntent = false);

  virtual ~ClientConnection();

  // Thrown by connect() when a server without challenge support can't reattach.
  static const string LEGACY_SERVER_REATTACH_ERROR;

  /**
   * @brief Attempts to establish and authenticate a connection to the server.
   * @return true when the connection handshake succeeded.
   */
  bool connect();

  /**
   * @brief Extends the base behavior to spawn a reconnect thread after closing.
   */
  virtual void closeSocketAndMaybeReconnect();

  // True when the last connect used the reset handshake.
  bool wasRecovered() const { return recovered_.load(); }

  et::ConnectStatus lastStatus() const { return lastStatus_.load(); }

  /**
   * @brief Blocks until any running reconnect thread has finished.
   */
  void waitReconnect();

 protected:
  void connectHandshake(int fd, et::ConnectResponse* response,
                        bool resetIntent);

  /**
   * @brief Background loop used to re-establish a connection when lost.
   */
  void pollReconnect();

  /** @brief Server endpoint we try to connect to. */
  SocketEndpoint remoteEndpoint;
  // Set when resuming a session from a process with no sequence history; the
  // first connect then asks the server to reset.
  const bool resetOnConnect;
  std::atomic<bool> recovered_{false};
  std::atomic<et::ConnectStatus> lastStatus_{et::ConnectStatus::NEW_CLIENT};
  /** @brief Thread that keeps retrying the handshake after disconnects. */
  std::shared_ptr<std::thread> reconnectThread;
};
}  // namespace et

#endif  // __ET_SERVER_CONNECTION__
