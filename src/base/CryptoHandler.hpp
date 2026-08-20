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
  /**
   * @brief Initializes libsodium, copies the provided key, and seeds the nonce.
   * @param key Exactly crypto_secretbox_KEYBYTES bytes of shared key material.
   * @param nonceMSB Most significant byte used to distinguish client/server
   * streams.
   */
  explicit CryptoHandler(const string& key, unsigned char nonceMSB);
  ~CryptoHandler();

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
  /**
   * @brief Restores the nonce to its initial value (base nonce with only the
   * MSB byte set). Used by the reset handshake: when both sides agree to
   * discard all buffered history and restart at sequence 0, their per-message
   * nonces must also restart from the base so the fresh streams stay in
   * lockstep.
   * @note This deliberately reuses nonces that were consumed before the reset.
   * That is only safe because the reset discards the old session's buffered
   * packets on both sides, so no two live ciphertexts share a nonce.
   */
  void resetNonce();

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
  /** @brief MSB byte used to seed the nonce (see constructor). */
  unsigned char nonceMSB;

 private:
  /** @brief Guards the nonce/key pair to keep operations thread-safe. */
  mutex cryptoMutex;
};
}  // namespace et

#endif  // __ET_CRYPTO_HANDLER__
