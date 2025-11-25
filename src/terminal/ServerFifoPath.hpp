#ifndef __ET_SERVER_FIFO_PATH__
#define __ET_SERVER_FIFO_PATH__

#include <optional>

#include "Headers.hpp"
#include "SocketHandler.hpp"

namespace et {

/**
 * A helper class to handle creating and detecting the server fifo path.
 *
 * The default fifo path location varies based on which user the etserver
 * process is running as, and it may also be overridden from a command line
 * flag.
 *
 * This class aggregates that logic, both on the server and client side.
 *
 * To use:
 * - Create the class, and optionally call \ref setPathOverride.
 * - On the creation side, call \ref createDirectoriesIfRequired and \ref
 *   getPathForCreation.
 * - On the client side, call \ref getEndpointForConnect and \ref
 *   detectAndConnect, which will either connect to the overridden path or try
 *   both the root location, followed by the non-root location of the fifo to
 *   connect. Since a broken fifo file can be left behind when the process
 *   exits, this tries to connect to each pipe in sequence and performs a
 *   graceful fallback.
 *
 * For root, the fifo is placed in the root-accessible directory /var/run.
 *
 * For non-root, this is placed in the user directory, under $HOME/.local/share,
 * following the XDG spec.  This class contains logic to create the
 * $HOME/.local/share directory structure if required.  This means that if the
 * server runs as a non-root user, it may only be connected by the same user.
 */
class ServerFifoPath {
 public:
  /** @brief Initializes helper state used by server/clients to locate the server fifo. */
  ServerFifoPath();

  /**
   * Overrides the fifo path to a user-specified location. Note that this
   * disables the auto-detection behavior.
   *
   * @param path User-specified path to the serverfifo.
   */
  void setPathOverride(string path);

  /**
   * Based on the current uid, create the directory structure required to store
   * the fifo once it is created.  If XDG_DATA_HOME is not set and the processes
   * user cannot access /var/run, this will ensure that $HOME/.local/share
   * exists.
   */
  void createDirectoriesIfRequired();

  /**
   * Get the computed fifo path to use when creating the fifo. This will return
   * the override path, or a location in either /var/run as root or
   * $HOME/.local/share otherwise.
   */
  string getPathForCreation();

  /**
   * Return an SocketEndpoint or nullopt based on the current configuration,
   * which may later be passed to \ref detectAndConnect to connect to the
   * relevant endpoint.
   */
  optional<SocketEndpoint> getEndpointForConnect();

  /**
   * Either connect to the specific router endpoint, if provided, or detect and
   * connect to the default root or non-root location of the endpoint.
   *
   * @return fd of the connected pipe, always valid. Exits internally if the
   *   pipe cannot be connected.
   */
  static int detectAndConnect(
      const optional<SocketEndpoint> specificRouterEndpoint,
      const shared_ptr<SocketHandler>& socketHandler);

 private:
  /** @brief User-overridden fifo path that bypasses auto-detection. */
  optional<string> pathOverride;
};

}  // namespace et

#endif  // __ET_SERVER_FIFO_PATH__
