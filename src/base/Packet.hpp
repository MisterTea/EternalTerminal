#ifndef __ET_PACKET_H__
#define __ET_PACKET_H__

#include "CryptoHandler.hpp"
#include "Headers.hpp"

namespace et {
class Packet {
 public:
  Packet() : encrypted(false), header(255) {}
  Packet(uint8_t _header, const string& _payload)
      : encrypted(false), header(_header), payload(_payload) {}
  Packet(bool _encrypted, uint8_t _header, const string& _payload)
      : encrypted(_encrypted), header(_header), payload(_payload) {}
  explicit Packet(const string& serializedPacket) {
    encrypted = serializedPacket[0];
    header = serializedPacket[1];
    payload = serializedPacket.substr(2);
  }

  void decrypt(shared_ptr<CryptoHandler> cryptoHandler) {
    if (encrypted) {
      encrypted = false;
      payload = cryptoHandler->decrypt(payload);
    } else {
      STFATAL << "Tried to decrypt a packet that wasn't encrypted";
    }
  }

  void encrypt(shared_ptr<CryptoHandler> cryptoHandler) {
    if (encrypted) {
      STFATAL << "Tried to encrypt a packet that was already encrypted";
    } else {
      encrypted = true;
      payload = cryptoHandler->encrypt(payload);
    }
  }

  bool isEncrypted() const { return encrypted; }
  uint8_t getHeader() const { return header; }
  string getPayload() const { return payload; }

  ssize_t length() const { return HEADER_SIZE + payload.length(); }

  string serialize() const {
    string s = "00" + payload;
    s[0] = uint8_t(encrypted);
    s[1] = header;
    return s;
  }

 protected:
  static const int HEADER_SIZE = 2;
  bool encrypted;
  uint8_t header;

  string payload;
};
}  // namespace et

#endif
