#include "CryptoHandler.hpp"

#define SODIUM_FAIL(X)                                         \
  {                                                            \
    int rc = (X);                                              \
    if ((rc) == -1) STFATAL << "Crypto Error: (" << rc << ")"; \
  }
namespace et {

namespace {
string resetDecisionMessage(const string& clientId, int protocolVersion,
                            const string& challenge, int status,
                            bool resetRequired, const string& resetSalt) {
  string message = "EternalTerminal reset decision v3";
  message.push_back('\0');
  message += to_string(protocolVersion);
  message.push_back('\0');
  message += clientId;
  message.push_back('\0');
  message += to_string(challenge.size());
  message.push_back('\0');
  message += challenge;
  message.push_back('\0');
  message += to_string(status);
  message.push_back('\0');
  message += resetRequired ? "1" : "0";
  message.push_back('\0');
  message += to_string(resetSalt.size());
  message.push_back('\0');
  message += resetSalt;
  return message;
}

string connectionProofMessage(const string& clientId, int protocolVersion,
                              const string& challenge, bool resetIntent) {
  string message = "EternalTerminal connect auth v2";
  message.push_back('\0');
  message += to_string(protocolVersion);
  message.push_back('\0');
  message += to_string(clientId.size());
  message.push_back('\0');
  message += clientId;
  message.push_back('\0');
  message += to_string(challenge.size());
  message.push_back('\0');
  message += challenge;
  message.push_back('\0');
  message += resetIntent ? "1" : "0";
  return message;
}

}  // namespace

string CryptoHandler::resetDecisionProof(const string& _key,
                                         const string& clientId,
                                         int protocolVersion,
                                         const string& challenge, int status,
                                         bool resetRequired,
                                         const string& resetSalt) {
  if (_key.length() != crypto_secretbox_KEYBYTES) {
    throw std::runtime_error("Invalid reset decision proof key length");
  }
  if (challenge.length() != AUTH_CHALLENGE_BYTES) {
    throw std::runtime_error("Invalid reset decision challenge length");
  }
  if ((resetRequired && resetSalt.length() != EPOCH_SALT_BYTES) ||
      (!resetRequired && !resetSalt.empty())) {
    throw std::runtime_error("Invalid reset decision salt");
  }
  string message = resetDecisionMessage(clientId, protocolVersion, challenge,
                                        status, resetRequired, resetSalt);
  string proof(crypto_generichash_BYTES, '\0');
  if (crypto_generichash(
          reinterpret_cast<unsigned char*>(&proof[0]), proof.length(),
          reinterpret_cast<const unsigned char*>(message.data()),
          message.length(), reinterpret_cast<const unsigned char*>(_key.data()),
          _key.length()) != 0) {
    throw std::runtime_error("Reset decision proof generation failed");
  }
  return proof;
}

bool CryptoHandler::verifyResetDecisionProof(
    const string& proof, const string& _key, const string& clientId,
    int protocolVersion, const string& challenge, int status,
    bool resetRequired, const string& resetSalt) {
  if (proof.length() != crypto_generichash_BYTES ||
      _key.length() != crypto_secretbox_KEYBYTES ||
      challenge.length() != AUTH_CHALLENGE_BYTES) {
    return false;
  }
  if ((resetRequired && resetSalt.length() != EPOCH_SALT_BYTES) ||
      (!resetRequired && !resetSalt.empty())) {
    return false;
  }
  const string expected =
      resetDecisionProof(_key, clientId, protocolVersion, challenge, status,
                         resetRequired, resetSalt);
  return sodium_memcmp(proof.data(), expected.data(), expected.length()) == 0;
}

CryptoHandler::CryptoHandler(const string& _key, unsigned char nonceMSB)
    : nonceMSB(nonceMSB) {
  lock_guard<std::mutex> guard(cryptoMutex);
  if (-1 == sodium_init()) {
    STFATAL << "libsodium init failed";
  }
  if (_key.length() != crypto_secretbox_KEYBYTES) {
    STFATAL << "Invalid key length";
  }
  memcpy(baseKey, &_key[0], _key.length());
  memcpy(key, &_key[0], _key.length());
  memset(nonce, 0, crypto_secretbox_NONCEBYTES);
  nonce[crypto_secretbox_NONCEBYTES - 1] = nonceMSB;
}

void CryptoHandler::rekey(const string& salt) {
  lock_guard<std::mutex> guard(cryptoMutex);
  if (salt.length() != EPOCH_SALT_BYTES) {
    throw std::runtime_error("Invalid epoch salt length");
  }
  if (crypto_generichash(key, crypto_secretbox_KEYBYTES,
                         reinterpret_cast<const unsigned char*>(salt.data()),
                         salt.length(), baseKey,
                         crypto_secretbox_KEYBYTES) != 0) {
    throw std::runtime_error("Epoch key derivation failed");
  }
  memset(nonce, 0, crypto_secretbox_NONCEBYTES);
  nonce[crypto_secretbox_NONCEBYTES - 1] = nonceMSB;
}

CryptoHandler::~CryptoHandler() {}

string CryptoHandler::randomBytes(size_t length) {
  if (-1 == sodium_init()) {
    throw std::runtime_error("libsodium init failed");
  }
  string bytes(length, '\0');
  randombytes_buf(&bytes[0], bytes.size());
  return bytes;
}

string CryptoHandler::connectionProof(const string& _key,
                                      const string& clientId,
                                      int protocolVersion,
                                      const string& challenge,
                                      bool resetIntent) {
  if (_key.length() != crypto_secretbox_KEYBYTES) {
    throw std::runtime_error("Invalid connection proof key length");
  }
  if (challenge.length() != AUTH_CHALLENGE_BYTES) {
    throw std::runtime_error("Invalid connection challenge length");
  }
  if (-1 == sodium_init()) {
    throw std::runtime_error("libsodium init failed");
  }

  const string message =
      connectionProofMessage(clientId, protocolVersion, challenge, resetIntent);
  string proof(crypto_generichash_BYTES, '\0');
  if (crypto_generichash(
          reinterpret_cast<unsigned char*>(&proof[0]), proof.length(),
          reinterpret_cast<const unsigned char*>(message.data()),
          message.length(), reinterpret_cast<const unsigned char*>(_key.data()),
          _key.length()) != 0) {
    throw std::runtime_error("Connection proof generation failed");
  }
  return proof;
}

bool CryptoHandler::verifyConnectionProof(
    const string& proof, const string& _key, const string& clientId,
    int protocolVersion, const string& challenge, bool resetIntent) {
  if (proof.length() != crypto_generichash_BYTES ||
      _key.length() != crypto_secretbox_KEYBYTES ||
      challenge.length() != AUTH_CHALLENGE_BYTES) {
    return false;
  }
  const string expected =
      connectionProof(_key, clientId, protocolVersion, challenge, resetIntent);
  return sodium_memcmp(proof.data(), expected.data(), expected.length()) == 0;
}

string CryptoHandler::encrypt(const string& buffer) {
  lock_guard<std::mutex> guard(cryptoMutex);
  incrementNonce();
  string retval(buffer.length() + crypto_secretbox_MACBYTES, '\0');
  SODIUM_FAIL(crypto_secretbox_easy((unsigned char*)&retval[0],
                                    (const unsigned char*)buffer.c_str(),
                                    buffer.length(), nonce, key));
  return retval;
}

string CryptoHandler::decrypt(const string& buffer) {
  lock_guard<std::mutex> guard(cryptoMutex);
  incrementNonce();
  string retval(buffer.length() - crypto_secretbox_MACBYTES, '\0');
  if (crypto_secretbox_open_easy((unsigned char*)&retval[0],
                                 (const unsigned char*)buffer.c_str(),
                                 buffer.length(), nonce, key) == -1) {
    throw std::runtime_error("Decrypt failed. Possible key mismatch?");
  }
  return retval;
}

void CryptoHandler::incrementNonce() {
  // Increment nonce
  for (int a = 0; a < int(crypto_secretbox_NONCEBYTES); a++) {
    nonce[a]++;
    if (nonce[a]) {
      // When nonce[a]==0, it means we rolled over to the next digit;
      break;
    }
  }
}
}  // namespace et
