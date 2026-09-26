#ifndef __ET_SERVER_CONNECTION__
#define __ET_SERVER_CONNECTION__

#include "Headers.hpp"
#include "ServerClientConnection.hpp"
#include "SocketHandler.hpp"

namespace et {
struct IdKeyPair {
  string id;
  string key;
};

/**
 * @brief Base class for servers that accept clients over sockets and track
 * them.
 *
 * Holds registered client keys and creates `ServerClientConnection` instances
 * for each authenticated client that connects.
 */
class ServerConnection {
 public:
  explicit ServerConnection(std::shared_ptr<SocketHandler> socketHandler,
                            const SocketEndpoint& _serverEndpoint);

  ~ServerConnection();

  inline bool clientKeyExists(const string& clientId) {
    lock_guard<std::recursive_mutex> guard(classMutex);
    return clientKeys.find(clientId) != clientKeys.end();
  }

  inline bool clientConnectionExists(const string& clientId) {
    lock_guard<std::recursive_mutex> guard(classMutex);
    return clientConnections.find(clientId) != clientConnections.end();
  }

  inline shared_ptr<SocketHandler> getSocketHandler() { return socketHandler; }

  /**
   * @brief Accepts a pending connection on the listening fd and starts a
   * handler.
   * @param fd Listening socket descriptor returned by `listen()`.
   */
  bool acceptNewConnection(int fd);

  /**
   * @brief Stops accepting new clients and shuts down existing connections.
   */
  void shutdown();

  inline void addClientKey(const string& id, const string& passkey) {
    lock_guard<std::recursive_mutex> guard(classMutex);
    clientKeys[id] = passkey;
    removedClientIds.erase(id);
  }

  /**
   * @brief Entry point invoked on the thread pool for each client connection.
   */
  void clientHandler(int clientSocketFd);

  /**
   * @brief Removes a registered client and terminates its active connection.
   */
  bool removeClient(const string& id);

  shared_ptr<ServerClientConnection> tryGetClientConnection(
      const string& clientId) {
    lock_guard<std::recursive_mutex> guard(classMutex);
    auto it = clientConnections.find(clientId);
    return it == clientConnections.end() ? nullptr : it->second;
  }

  shared_ptr<ServerClientConnection> getClientConnection(
      const string& clientId) {
    auto connection = tryGetClientConnection(clientId);
    if (!connection) {
      STFATAL << "Error: Tried to get a client connection that doesn't exist";
    }
    return connection;
  }

  /**
   * @brief Callback that derived classes use to integrate newly authenticated
   *        clients into higher level server behavior.
   */
  virtual bool newClient(
      shared_ptr<ServerClientConnection> serverClientState) = 0;

  // A known id whose terminal re-registered with a live pty is resumed
  // instead of bootstrapped.
  virtual bool shouldResumeAsReturning(const string& clientId) { return false; }
  virtual void resumeClient(shared_ptr<ServerClientConnection> state) {}

 protected:
  bool authenticateClient(int clientSocketFd, const string& clientId,
                          const string& clientKey, int protocolVersion,
                          bool resetIntent, string* challengeOut);

  /**
   * @brief Discards a partially initialized connection if its thread fails.
   */
  void destroyPartialConnection(const string& clientId);

  /** @brief Socket helper used by the server. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief Endpoint the server listens on. */
  SocketEndpoint serverEndpoint;
  /** @brief Map of client IDs to their registered passkeys. */
  std::unordered_map<string, string> clientKeys;
  // Client ids whose sessions ended in this process, with removal time.
  std::unordered_map<string, time_t> removedClientIds;
  /** @brief Active client connections indexed by ID. */
  std::unordered_map<string, shared_ptr<ServerClientConnection>>
      clientConnections;
  /** @brief Thread pool used to handle incoming client sockets. */
  std::unique_ptr<ThreadPool> clientHandlerThreadPool;
  /** @brief Guards server state, including the client maps. */
  recursive_mutex classMutex;
  /** @brief Serializes connect/disconnect events. */
  mutex connectMutex;
  // After a restart, unknown ids get RETRY_LATER instead of INVALID_KEY for
  // this long so terminals can re-register.
  int recoveryGraceSeconds = 60;
  time_t startTime_;

  void pruneRemovedClientIds(time_t now);
};
}  // namespace et

#endif  // __ET_SERVER_CONNECTION__
