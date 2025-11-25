#ifndef __ET_BACKED_READER__
#define __ET_BACKED_READER__

#include "CryptoHandler.hpp"
#include "Headers.hpp"
#include "Packet.hpp"
#include "SocketHandler.hpp"

namespace et {
/**
 * @brief Reads packets from a socket while preserving enough state to replay
 *        any messages after reconnecting.
 */
class BackedReader {
 public:
  /**
   * @brief Constructs a reader bound to the supplied socket and crypto handlers.
   * @param socketHandler Handler that performs the underlying socket operations.
   * @param cryptoHandler Handler used to decrypt packets once they are received.
   * @param socketFd Initial socket file descriptor to read from.
   */
  BackedReader(shared_ptr<SocketHandler> socketHandler,
               shared_ptr<CryptoHandler> cryptoHandler, int socketFd);

  /**
   * @brief Returns true if there is buffered data or the current socket is readable.
   */
  bool hasData();

  /**
   * @brief Reads the next packet from the local buffer or socket, decrypting it.
   * @param packet Output packet that is filled when the read completes.
   * @return 1 when a complete packet was read, 0 if more bytes are required,
   *         and -1 on fatal socket error.
   */
  int read(Packet* packet);

  /**
   * @brief Exposes the mutex guarding recovery mutators so callers can synchronize.
   */
  mutex& getRecoverMutex() { return recoverMutex; }

  /**
   * @brief Resumes the reader on a new socket and queues any serialized packets
   *        that should be replayed before fresh reads.
   * @param newSocketFd New active socket file descriptor.
   * @param newLocalEntries Serialized packets buffered while reconnecting.
   */
  void revive(int newSocketFd, const vector<string>& newLocalEntries);

  /**
   * @brief Marks the reader as disconnected so callers stop issuing reads.
   */
  inline void invalidateSocket() {
    lock_guard<std::mutex> guard(recoverMutex);
    socketFd = -1;
  }

  /**
   * @brief Returns the number of packets digested so far (used for recovery tracking).
   */
  inline int64_t getSequenceNumber() { return sequenceNumber; }

 protected:
  /** @brief Guards socket and buffer mutations when recovering state. */
  mutex recoverMutex;
  /** @brief Handler that interfaces with the platform socket API. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief Responsible for decrypting packets once they arrive. */
  shared_ptr<CryptoHandler> cryptoHandler;
  /** @brief Current socket file descriptor (-1 when disconnected). */
  volatile int socketFd;
  /** @brief Packet sequence counter that increments for every read packet. */
  int64_t sequenceNumber;
  /** @brief Serialized packets cached to be drained before resuming live reads. */
  deque<string> localBuffer;
  /** @brief Buffer for accumulating length-prefixed packet data from the socket. */
  string partialMessage;

  /**
   * @brief Helper that resets sequence tracking and clears buffered data.
   * @param firstSequenceNumber Sequence number to start counting from.
   */
  void init(int64_t firstSequenceNumber);

  /**
   * @brief Parses the queued header bytes to determine the payload length.
   * @return Expected payload size derived from the 4-byte length prefix.
   */
  int getPartialMessageLength();

  /**
   * @brief Finalizes a complete message stored in {@link partialMessage} and
   *        decrypts it into the provided packet.
   */
  void constructPartialMessage(Packet* packet);
};
}  // namespace et

#endif  // __ET_BACKED_READER__
