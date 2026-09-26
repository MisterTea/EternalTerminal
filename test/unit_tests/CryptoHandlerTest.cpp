#include "CryptoHandler.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("DoesEncryptDecrypt", "[CryptoHandler]") {
  string key = "12345678901234567890123456789012";
  shared_ptr<CryptoHandler> encryptHandler(new CryptoHandler(key, 0));
  shared_ptr<CryptoHandler> decryptHandler(new CryptoHandler(key, 0));
  string message = "ET Phone Home";
  string encryptedMessage = encryptHandler->encrypt(message);
  REQUIRE(message != encryptedMessage);
  string decryptedMessage = decryptHandler->decrypt(encryptedMessage);
  REQUIRE(message == decryptedMessage);
}

TEST_CASE("Reset epochs do not reuse ciphertext", "[CryptoHandler]") {
  const string key = "12345678901234567890123456789012";
  const string firstSalt(CryptoHandler::EPOCH_SALT_BYTES, 'a');
  const string secondSalt(CryptoHandler::EPOCH_SALT_BYTES, 'b');
  const string message = "same sequence and plaintext";
  CryptoHandler writer(key, 0);
  CryptoHandler reader(key, 0);
  CryptoHandler mismatchedReader(key, 0);

  const string initialEpoch = writer.encrypt(message);
  writer.rekey(firstSalt);
  reader.rekey(firstSalt);
  const string firstResetEpoch = writer.encrypt(message);
  REQUIRE(reader.decrypt(firstResetEpoch) == message);

  writer.rekey(secondSalt);
  const string secondResetEpoch = writer.encrypt(message);

  REQUIRE(initialEpoch != firstResetEpoch);
  REQUIRE(firstResetEpoch != secondResetEpoch);

  mismatchedReader.rekey(secondSalt);
  REQUIRE_THROWS_AS(mismatchedReader.decrypt(firstResetEpoch),
                    std::runtime_error);
}

TEST_CASE("Connection proofs bind fresh challenges", "[CryptoHandler]") {
  const string key = "12345678901234567890123456789012";
  const string clientId = "client-id";
  const string firstChallenge =
      CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES);
  const string secondChallenge =
      CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES);

  REQUIRE(firstChallenge.size() == CryptoHandler::AUTH_CHALLENGE_BYTES);
  REQUIRE(secondChallenge.size() == CryptoHandler::AUTH_CHALLENGE_BYTES);
  REQUIRE(firstChallenge != secondChallenge);

  const string firstProof = CryptoHandler::connectionProof(
      key, clientId, PROTOCOL_VERSION, firstChallenge);
  REQUIRE(CryptoHandler::verifyConnectionProof(
      firstProof, key, clientId, PROTOCOL_VERSION, firstChallenge));
  REQUIRE_FALSE(CryptoHandler::verifyConnectionProof(
      firstProof, key, clientId, PROTOCOL_VERSION, secondChallenge));
  REQUIRE_FALSE(CryptoHandler::verifyConnectionProof(
      firstProof, key, "other-client", PROTOCOL_VERSION, firstChallenge));
}

TEST_CASE("Reset decision proofs bind the exact reset salt",
          "[CryptoHandler]") {
  const string key = "12345678901234567890123456789012";
  const string clientId = "client-id";
  const string challenge =
      CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES);
  const string firstSalt(CryptoHandler::EPOCH_SALT_BYTES, 'a');
  const string secondSalt(CryptoHandler::EPOCH_SALT_BYTES, 'b');

  const string proof = CryptoHandler::resetDecisionProof(
      key, clientId, PROTOCOL_VERSION, challenge, RETURNING_CLIENT, true,
      firstSalt);
  REQUIRE(CryptoHandler::verifyResetDecisionProof(
      proof, key, clientId, PROTOCOL_VERSION, challenge, RETURNING_CLIENT, true,
      firstSalt));
  REQUIRE_FALSE(CryptoHandler::verifyResetDecisionProof(
      proof, key, clientId, PROTOCOL_VERSION, challenge, RETURNING_CLIENT, true,
      secondSalt));
  REQUIRE_FALSE(CryptoHandler::verifyResetDecisionProof(
      proof, key, clientId, PROTOCOL_VERSION, challenge, RETURNING_CLIENT,
      false, string()));
  REQUIRE_THROWS_AS(CryptoHandler::resetDecisionProof(
                        key, clientId, PROTOCOL_VERSION, challenge,
                        RETURNING_CLIENT, true, string()),
                    std::runtime_error);
}

TEST_CASE("Reset decision proofs bind the fresh server challenge and status",
          "[CryptoHandler]") {
  const string key = "12345678901234567890123456789012";
  const string clientId = "client-id";
  const string firstChallenge =
      CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES);
  const string secondChallenge =
      CryptoHandler::randomBytes(CryptoHandler::AUTH_CHALLENGE_BYTES);
  const string salt(CryptoHandler::EPOCH_SALT_BYTES, 'a');

  REQUIRE(firstChallenge != secondChallenge);

  const string proof = CryptoHandler::resetDecisionProof(
      key, clientId, PROTOCOL_VERSION, firstChallenge, RETURNING_CLIENT, true,
      salt);
  REQUIRE(CryptoHandler::verifyResetDecisionProof(
      proof, key, clientId, PROTOCOL_VERSION, firstChallenge, RETURNING_CLIENT,
      true, salt));
  REQUIRE_FALSE(CryptoHandler::verifyResetDecisionProof(
      proof, key, clientId, PROTOCOL_VERSION, secondChallenge, RETURNING_CLIENT,
      true, salt));

  REQUIRE_FALSE(CryptoHandler::verifyResetDecisionProof(
      proof, key, clientId, PROTOCOL_VERSION, firstChallenge, NEW_CLIENT, true,
      salt));

  REQUIRE_THROWS_AS(
      CryptoHandler::resetDecisionProof(key, clientId, PROTOCOL_VERSION,
                                        string(), RETURNING_CLIENT, true, salt),
      std::runtime_error);
}
