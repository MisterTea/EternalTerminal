#include "Headers.hpp"

#include "gtest/gtest.h"

#include "CryptoHandler.hpp"

using namespace et;

TEST(A, B) { SUCCEED(); }

TEST(CryptoHandler, DoesEncryptDecrypt) {
  string key = "12345678901234567890123456789012";
  shared_ptr<CryptoHandler> encryptHandler(new CryptoHandler(key));
  shared_ptr<CryptoHandler> decryptHandler(new CryptoHandler(key));
  string message = "ET Phone Home";
  string encryptedMessage = encryptHandler->encrypt(message);
  EXPECT_NE(message, encryptedMessage);
  string decryptedMessage = decryptHandler->decrypt(encryptedMessage);
  EXPECT_EQ(message, decryptedMessage);
}

int main(int argc, char **argv) {
  srand(1);
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
