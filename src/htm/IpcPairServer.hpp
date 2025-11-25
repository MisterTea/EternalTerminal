#ifndef __IPC_PAIR_SERVER_H__
#define __IPC_PAIR_SERVER_H__

#include "Headers.hpp"
#include "IpcPairEndpoint.hpp"

namespace et {
/**
 * @brief Server-side helper that listens on a pipe and accepts a single endpoint.
 */
class IpcPairServer : public IpcPairEndpoint {
 public:
  /**
   * @brief Binds a listening socket for HTM clients that this server accepts.
   */
  IpcPairServer(shared_ptr<SocketHandler> _socketHandler,
                const SocketEndpoint &_endpoint);
  /** @brief Tears down listeners and closes the server fd. */
  virtual ~IpcPairServer();
  /** @brief Accepts a new HTM client and stores its descriptor in `endpointFd`. */
  virtual void pollAccept();
  /** @brief Accessor for the server's listening descriptor. */
  inline int getServerFd() { return serverFd; }

  /** @brief Subclasses must supply recovery logic when clients reconnect. */
  virtual void recover() = 0;

 protected:
  /** @brief Listening socket descriptor for new HTM clients. */
  int serverFd;
  /** @brief Pipe endpoint used for HTM communication. */
  SocketEndpoint endpoint;
};
}  // namespace et

#endif  // __IPC_PAIR_SERVER_H__
