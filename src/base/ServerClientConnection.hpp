#ifndef __ET_SERVER_CLIENT_CONNECTION__
#define __ET_SERVER_CLIENT_CONNECTION__

#include "Connection.hpp"
#include "Headers.hpp"

namespace et {
/**
 * @brief Represents the server-side state for a single authenticated client.
 *
 * Allows a reconnecting client to replay buffered packets and validates
 * passkeys without exposing timing differences.
 */
class ServerClientConnection : public Connection {
 public:
  explicit ServerClientConnection(
      const std::shared_ptr<SocketHandler>& _socketHandler,
      const string& clientId, int _socketFd, const string& key);

  virtual ~ServerClientConnection();

  /**
   * @brief Attempts recovery on the new fd; closes the old socket only after
   * recover succeeds.
   *
   * Returns false without touching the live session if another reconnect for
   * this client is already in flight.
   */
  bool recoverClient(int newSocketFd);

  /**
   * @brief Constant-time comparison of the stored key and a supplied passkey.
   */
  bool verifyPasskey(const string& targetKey);

 protected:
  /**
   * @brief Set while a reconnect is mid-handshake.
   *
   * Lets a second reconnect be refused rather than queued, since queueing it
   * would occupy a handler thread for as long as the first one blocks.
   */
  std::atomic<bool> recoveryInFlight;
};
}  // namespace et

#endif  // __ET_SERVER_CLIENT_CONNECTION__
