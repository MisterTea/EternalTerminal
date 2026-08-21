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
