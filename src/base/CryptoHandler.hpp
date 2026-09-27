#ifndef __ET_CRYPTO_HANDLER__
#define __ET_CRYPTO_HANDLER__

#include <sodium.h>

#include "Headers.hpp"

namespace et {

/**
 * @brief Provides thread-safe libsodium secretbox encryption/decryption state.
 */
class CryptoHandler {
 public:
  static constexpr size_t EPOCH_SALT_BYTES = 32;
  static constexpr size_t AUTH_CHALLENGE_BYTES = 32;

  /**
   * @brief Initializes libsodium, copies the provided key, and seeds the nonce.
   * @param key Exactly crypto_secretbox_KEYBYTES bytes of shared key material.
   * @param nonceMSB Most significant byte used to distinguish client/server
   * streams.
   */
  explicit CryptoHandler(const string& key, unsigned char nonceMSB);
  ~CryptoHandler();

  static string randomBytes(size_t length);
  static string connectionProof(const string& key, const string& clientId,
                                int protocolVersion, const string& challenge,
                                bool resetIntent = false);
  static bool verifyConnectionProof(const string& proof, const string& key,
                                    const string& clientId, int protocolVersion,
                                    const string& challenge,
                                    bool resetIntent = false);
  // Binds the server's status and reset decision to this handshake's
  // challenge so a captured ConnectResponse can't be replayed.
  static string resetDecisionProof(const string& key, const string& clientId,
                                   int protocolVersion, const string& challenge,
                                   int status, bool resetRequired,
                                   const string& resetSalt);
  static bool verifyResetDecisionProof(const string& proof, const string& key,
                                       const string& clientId,
                                       int protocolVersion,
                                       const string& challenge, int status,
                                       bool resetRequired,
                                       const string& resetSalt);

  /**
   * @brief Encrypts a plaintext buffer and advances the nonce.
   * @param buffer Plaintext payload to seal with secretbox.
   * @return Ciphertext including the MAC.
   */
  string encrypt(const string& buffer);

  /**
   * @brief Decrypts a ciphertext buffer and advances the nonce.
   * @param buffer Ciphertext that must contain the MAC.
   * @return Original plaintext payload.
   */
  string decrypt(const string& buffer);

  // Derives a new epoch key from the original key and resets the nonce.
  void rekey(const string& salt);

 protected:
  /**
   * @brief Increments the nonce to guarantee a unique per-message secretbox
   * input.
   */
  void incrementNonce();
  /** @brief Nonce used for the next encryption/decryption call. */
  unsigned char nonce[crypto_secretbox_NONCEBYTES];
  /** @brief Shared secret key used for encrypt/decrypt operations. */
  unsigned char key[crypto_secretbox_KEYBYTES];
  unsigned char baseKey[crypto_secretbox_KEYBYTES];
  unsigned char nonceMSB;

 private:
  /** @brief Guards the nonce/key pair to keep operations thread-safe. */
  mutex cryptoMutex;
};
}  // namespace et

#endif  // __ET_CRYPTO_HANDLER__
