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
   * @brief Tears down the old socket (if any) and attempts recovery on the new fd.
   */
  bool recoverClient(int newSocketFd);

  /**
   * @brief Constant-time comparison of the stored key and a supplied passkey.
   */
  bool verifyPasskey(const string& targetKey);

 protected:
};
}  // namespace et

#endif  // __ET_SERVER_CLIENT_CONNECTION__
