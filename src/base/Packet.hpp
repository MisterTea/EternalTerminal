#ifndef __ET_PACKET_H__
#define __ET_PACKET_H__

#include "CryptoHandler.hpp"
#include "Headers.hpp"

namespace et {
/**
 * @brief Represents a length-encoded protocol packet with optional encryption.
 */
class Packet {
 public:
  /** @brief Constructs an empty, decrypted packet. */
  Packet() : encrypted(false), header(255) {}
  /**
   * @brief Builds an unencrypted packet from the given header/payload tuple.
   */
  Packet(uint8_t _header, const string& _payload)
      : encrypted(false), header(_header), payload(_payload) {}
  /**
   * @brief Allows callers to explicitly set the encrypted flag when constructing.
   */
  Packet(bool _encrypted, uint8_t _header, const string& _payload)
      : encrypted(_encrypted), header(_header), payload(_payload) {}
  /**
   * @brief Deserializes a packet from its raw byte representation.
   */
  explicit Packet(const string& serializedPacket) {
    encrypted = serializedPacket[0];
    header = serializedPacket[1];
    payload = serializedPacket.substr(2);
  }

  /**
   * @brief Decrypts the payload if the encrypted flag is set.
   * @param cryptoHandler Handler used to decrypt the blob.
   */
  void decrypt(shared_ptr<CryptoHandler> cryptoHandler) {
    if (encrypted) {
      encrypted = false;
      payload = cryptoHandler->decrypt(payload);
    } else {
      STFATAL << "Tried to decrypt a packet that wasn't encrypted";
    }
  }

  /**
   * @brief Encrypts the payload and tags the packet as encrypted.
   * @param cryptoHandler Handler used to encrypt the blob.
   */
  void encrypt(shared_ptr<CryptoHandler> cryptoHandler) {
    if (encrypted) {
      STFATAL << "Tried to encrypt a packet that was already encrypted";
    } else {
      encrypted = true;
      payload = cryptoHandler->encrypt(payload);
    }
  }

  /** @brief Returns true if the payload is currently encrypted. */
  bool isEncrypted() const { return encrypted; }
  /** @brief Retrieves the application-specific header byte. */
  uint8_t getHeader() const { return header; }
  /** @brief Returns the stored payload (decrypted if needed). */
  string getPayload() const { return payload; }

  /** @brief Returns the serialized byte count including the header. */
  ssize_t length() const { return HEADER_SIZE + payload.length(); }

  /**
   * @brief Serializes the header byte and payload into the packet wire format.
   * @return Byte string ready to be sent over the network.
   */
  string serialize() const {
    string s = "00" + payload;
    s[0] = uint8_t(encrypted);
    s[1] = header;
    return s;
  }

  protected:
  /** @brief Size of the non-payload portion of the serialized packet. */
  static const int HEADER_SIZE = 2;
  /** @brief Tracks whether the payload has been encrypted. */
  bool encrypted;
  /** @brief Application-specific packet type value stored as a byte. */
  uint8_t header;

  /** @brief Message body, encrypted or decrypted depending on the flag. */
  string payload;
};
}  // namespace et

#endif
