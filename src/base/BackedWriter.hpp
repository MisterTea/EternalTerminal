#ifndef __ET_BACKED_WRITER__
#define __ET_BACKED_WRITER__

#include "CryptoHandler.hpp"
#include "Headers.hpp"
#include "Packet.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Describes whether a write succeeded, was skipped, or partially lost
 * data.
 */
enum class BackedWriterWriteState {
  /** @brief Write attempt skipped because no socket is available. */
  SKIPPED = 0,  //
  /** @brief All bytes were transmitted successfully. */
  SUCCESS = 1,
  /** @brief Some bytes were written but the socket failed before completion. */
  WROTE_WITH_FAILURE = 2,
  /** @brief Packet was buffered but no socket available to send. */
  BUFFERED_ONLY = 3
};

/**
 * @brief Writes packets to a socket while maintaining an in-memory backup for
 * recovery.
 */
class BackedWriter {
 public:
  /** @brief Maximum bytes to buffer for recovery (64MB). */
  static const int64_t MAX_BACKUP_BYTES = 64 * 1024 * 1024;
  /** @brief Max buffer before blocking when disconnected (4MB). */
  static const int64_t DISCONNECT_BUFFER_BYTES = 4 * 1024 * 1024;

  /**
   * @brief Creates a writer bound to a socket and crypto pair.
   * @param socketFd Initial socket fd (-1 if not yet connected)
   */
  BackedWriter(shared_ptr<SocketHandler> socketHandler,
               shared_ptr<CryptoHandler> cryptoHandler, int socketFd);

  /**
   * @brief Encrypts and transmits the packet while keeping a backup copy.
   * @return State describing whether bytes were fully sent or buffered for
   * recovery.
   */
  BackedWriterWriteState write(Packet packet);

  /**
   * @brief Returns serialized packets that the remote side still needs after
   * reconnect.
   * @param lastValidSequenceNumber Sequence number acknowledged by the remote
   * peer.
   */
  vector<std::string> recover(int64_t lastValidSequenceNumber);

  /**
   * @brief Points the writer at a new socket fd so writes can resume.
   */
  void revive(int newSocketFd);

  /**
   * @brief Mutex guarding recovery operations so callers can hold it when
   * needed.
   */
  mutex& getRecoverMutex() { return recoverMutex; }

  /**
   * @brief Retrieves the fd currently being used for outbound writes.
   */
  inline int getSocketFd() { return socketFd; }

  /**
   * @brief Marks the current socket dead to prevent additional writes.
   */
  inline void invalidateSocket() {
    lock_guard<std::mutex> guard(recoverMutex);
    socketFd = -1;
  }

  /**
   * @brief Returns the total number of packets written since construction.
   */
  inline int64_t getSequenceNumber() { return sequenceNumber; }

 protected:
  /** @brief Synchronizes access to socket state and backup buffer. */
  mutex recoverMutex;
  /** @brief Platform socket helper. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief Encryption helper used before storing packets. */
  shared_ptr<CryptoHandler> cryptoHandler;
  /** @brief Current socket file descriptor for writes. */
  int socketFd;

  /** @brief Buffer of encrypted packets that may need to be replayed. */
  std::deque<Packet> backupBuffer;
  /** @brief Running size of the backup buffer. */
  int64_t backupSize;
  /** @brief Sequence number that increments each time a packet is backed up. */
  int64_t sequenceNumber;
};
}  // namespace et

#endif  // __ET_BACKED_WRITER__
