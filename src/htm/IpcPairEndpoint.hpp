#ifndef __IPC_PAIR_ENDPOINT_H__
#define __IPC_PAIR_ENDPOINT_H__

#include "Headers.hpp"
#include "HtmHeaderCodes.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Shared base for HTM IPC endpoints that hold a single pipe descriptor.
 *
 * Provides helper for sending the `SESSION_END` header when closing the pipe.
 */
class IpcPairEndpoint {
 public:
  /**
   * @brief Associates the shared socket handler with the specified pipe fd.
   */
  IpcPairEndpoint(shared_ptr<SocketHandler> _socketHandler, int _endpointFd);
  /** @brief Ensures the IPC descriptor is closed when the endpoint is
   * destroyed. */
  virtual ~IpcPairEndpoint();
  /** @brief Returns the currently tracked pipe descriptor. */
  inline int getEndpointFd() { return endpointFd; }
  /** @brief Sends `SESSION_END` to the peer before closing the descriptor. */
  virtual void closeEndpoint() {
    LOG(INFO) << "SENDING SESSION END";
    unsigned char header = SESSION_END;
    socketHandler->writeAllOrThrow(endpointFd, (const char *)&header, 1, false);
    socketHandler->close(endpointFd);
    endpointFd = -1;
  }

 protected:
  /** @brief Socket helper used for pipe reads/writes. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief Active descriptor shared between the pair. */
  int endpointFd;
};
}  // namespace et

#endif  // __IPC_PAIR_SERVER_H__
