#ifndef __ET_CONNECTION__
#define __ET_CONNECTION__

#include "BackedReader.hpp"
#include "BackedWriter.hpp"
#include "Headers.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Represents a buffered client/server connection with automatic recovery.
 *
 * Connection owns {@link BackedReader} and {@link BackedWriter} instances to
 * read/write encrypted packets while keeping replay buffers for reconnects.
 */
class Connection {
 public:
  /**
   * @brief Creates the shared reader/writer pair and assigns the connection id/key.
   */
  Connection(shared_ptr<SocketHandler> _socketHandler, const string& _id,
             const string& _key);

  virtual ~Connection();

  /**
   * @brief Thread-safe entry point that reads a packet if available.
   */
  virtual bool readPacket(Packet* packet);
  /**
   * @brief Repeatedly writes a packet until success or shutdown.
   */
  virtual void writePacket(const Packet& packet);

  /**
   * @brief Attempts to read one packet without looping.
   */
  virtual bool read(Packet* packet);
  /**
   * @brief Tries to write once and returns whether the call succeeded.
   */
  virtual bool write(const Packet& packet);

  inline shared_ptr<BackedReader> getReader() { return reader; }
  inline shared_ptr<BackedWriter> getWriter() { return writer; }

  /** @brief File descriptor of the currently connected socket or -1. */
  int getSocketFd() { return socketFd; }

  inline shared_ptr<SocketHandler> getSocketHandler() { return socketHandler; }

  inline bool isDisconnected() { return socketFd == -1; }

  inline string getId() { return id; }

  inline bool hasData() { return reader->hasData(); }

  /**
   * @brief Closes the socket and invalidates the reader/writer.
   */
  virtual void closeSocket();
  /**
   * @brief Override to trigger reconnect behavior after closing the socket.
   *        The default simply closes the socket and does not reconnect.
   */
  virtual void closeSocketAndMaybeReconnect() {
    closeSocket();
  }

  /**
   * @brief Signals that the connection should stop and tears down resources.
   */
  void shutdown();

  inline bool isShuttingDown() {
    lock_guard<std::recursive_mutex> guard(connectionMutex);
    return shuttingDown;
  }

  protected:
  /**
   * @brief Exchanges sequence headers and catchup buffers with a peer.
   * @return true if recovery succeeds and the new socket is owned by this object.
   */
  bool recover(int newSocketFd);

  /** @brief Socket API used by all derived connection types. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief Logical identifier for this connection (client ID for clients). */
  string id;
  /** @brief Shared secret key used to seed per-direction crypto handlers. */
  string key;
  /** @brief Reader that understands reconnect buffers. */
  std::shared_ptr<BackedReader> reader;
  /** @brief Writer that records packets for replay on reconnect. */
  std::shared_ptr<BackedWriter> writer;
  /** @brief Active socket descriptor, -1 when no connection exists. */
  int socketFd;
  /** @brief Flag that is set when `shutdown()` has been called. */
  bool shuttingDown;
  /** @brief Guards connection state changes in multi-threaded scenarios. */
  recursive_mutex connectionMutex;
};
}  // namespace et

#endif  // __ET_CONNECTION__
