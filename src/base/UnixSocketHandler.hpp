#ifndef __ET_UNIX_SOCKET_HANDLER__
#define __ET_UNIX_SOCKET_HANDLER__

#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Default SocketHandler implementation using POSIX sockets with mutex
 * guards.
 */
class UnixSocketHandler : public SocketHandler {
 public:
  UnixSocketHandler();
  virtual ~UnixSocketHandler() {}

  /**
   * @brief Blocks with select() until the fd becomes readable.
   */
  virtual bool waitForData(int fd, int64_t sec, int64_t usec);
  /** @brief Queries whether the descriptor currently has readable bytes. */
  virtual bool hasData(int fd);
  /** @brief Reads up to `count` bytes while holding the per-socket mutex. */
  virtual ssize_t read(int fd, void* buf, size_t count);
  /** @brief Writes `count` bytes by retrying until completion or timeout. */
  virtual ssize_t write(int fd, const void* buf, size_t count);
  /**
   * @brief Accepts a pending connection on the provided listening socket.
   */
  virtual int accept(int fd);
  /** @brief Closes the descriptor and removes it from the tracked set. */
  virtual void close(int fd);
  /** @brief Returns all actively tracked sockets. */
  virtual vector<int> getActiveSockets();

 protected:
  /**
   * @brief Ensures that a descriptor is tracked and has its own mutex.
   */
  void addToActiveSockets(int fd);
  /**
   * @brief Performs per-socket initialization (non-blocking, signal handling).
   */
  virtual void initSocket(int fd);
  /**
   * @brief Adds reusable flags for listening sockets.
   */
  virtual void initServerSocket(int fd);
  /**
   * @brief Toggles blocking mode on a descriptor.
   */
  void setBlocking(int sockFd, bool blocking);

  /** @brief Mutex per active socket to ensure serial read/write. */
  map<int, shared_ptr<recursive_mutex>> activeSocketMutexes;
  /** @brief Guards access to the active socket map. */
  recursive_mutex globalMutex;
};
}  // namespace et

#endif  // __ET_UNIX_SOCKET_HANDLER__
