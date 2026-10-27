#define SI_CONVERT_GENERIC
#include "../SimpleIni.h"
#include "gtest/gtest.h"

// Test SI_CONVERT_GENERIC with char interface
class TestGenericChar : public ::testing::Test {
protected:
  void SetUp() override;

protected:
  CSimpleIniA ini;
};

void TestGenericChar::SetUp() {
  ini.SetUnicode();
  SI_Error err = ini.LoadFile("tests.ini");
  ASSERT_EQ(err, SI_OK);
}

TEST_F(TestGenericChar, TestSectionAKeyAValA) {
  const char *result = ini.GetValue("section1", "key1");
  ASSERT_STREQ(result, "value1");
}

TEST_F(TestGenericChar, TestSectionAKeyAValU) {
  const char tesuto2[] = u8"テスト2";
  const char *result = ini.GetValue("section2", "test2");
  ASSERT_STREQ(result, tesuto2);
}

TEST_F(TestGenericChar, TestSectionAKeyUValA) {
  const char tesuto[] = u8"テスト";
  const char *result = ini.GetValue("section2", tesuto);
  ASSERT_STREQ(result, "test");
}

TEST_F(TestGenericChar, TestSectionAKeyUValU) {
  const char tesuto2[] = u8"テスト2";
  const char tesutoni[] = u8"テスト二";
  const char *result = ini.GetValue("section2", tesuto2);
  ASSERT_STREQ(result, tesutoni);
}

TEST_F(TestGenericChar, TestSectionUKeyAValA) {
  const char kensa[] = u8"検査";
  const char *result = ini.GetValue(kensa, "key2");
  ASSERT_STREQ(result, "value2");
}

TEST_F(TestGenericChar, TestSectionUKeyAValU) {
  const char kensa[] = u8"検査";
  const char tesuto2[] = u8"テスト2";
  const char *result = ini.GetValue(kensa, "test2");
  ASSERT_STREQ(result, tesuto2);
}

TEST_F(TestGenericChar, TestSectionUKeyUValA) {
  const char kensa[] = u8"検査";
  const char tesuto[] = u8"テスト";
  const char *result = ini.GetValue(kensa, tesuto);
  ASSERT_STREQ(result, "test");
}

TEST_F(TestGenericChar, TestSectionUKeyUValU) {
  const char kensa[] = u8"検査";
  const char tesuto2[] = u8"テスト2";
  const char tesutoni[] = u8"テスト二";
  const char *result = ini.GetValue(kensa, tesuto2);
  ASSERT_STREQ(result, tesutoni);
}

#ifdef _WIN32
// Test SI_CONVERT_GENERIC with wchar_t interface (Windows only)
// On non-Windows platforms, wchar_t support with SI_CONVERT_GENERIC doesn't work the same way
class TestGenericWide : public ::testing::Test {
protected:
  void SetUp() override;

protected:
  CSimpleIniW ini;
};

void TestGenericWide::SetUp() {
  ini.SetUnicode();
  SI_Error err = ini.LoadFile("tests.ini");
  ASSERT_EQ(err, SI_OK);
}

TEST_F(TestGenericWide, TestSectionAKeyAValA) {
  const wchar_t *result = ini.GetValue(L"section1", L"key1");
  ASSERT_STREQ(result, L"value1");
}

TEST_F(TestGenericWide, TestSectionAKeyAValU) {
  const wchar_t tesuto2[] = L"テスト2";
  const wchar_t *result = ini.GetValue(L"section2", L"test2");
  ASSERT_STREQ(result, tesuto2);
}

TEST_F(TestGenericWide, TestSectionAKeyUValA) {
  const wchar_t tesuto[] = L"テスト";
  const wchar_t *result = ini.GetValue(L"section2", tesuto);
  ASSERT_STREQ(result, L"test");
}

TEST_F(TestGenericWide, TestSectionAKeyUValU) {
  const wchar_t tesuto2[] = L"テスト2";
  const wchar_t tesutoni[] = L"テスト二";
  const wchar_t *result = ini.GetValue(L"section2", tesuto2);
  ASSERT_STREQ(result, tesutoni);
}

TEST_F(TestGenericWide, TestSectionUKeyAValA) {
  const wchar_t kensa[] = L"検査";
  const wchar_t *result = ini.GetValue(kensa, L"key2");
  ASSERT_STREQ(result, L"value2");
}

TEST_F(TestGenericWide, TestSectionUKeyAValU) {
  const wchar_t kensa[] = L"検査";
  const wchar_t tesuto2[] = L"テスト2";
  const wchar_t *result = ini.GetValue(kensa, L"test2");
  ASSERT_STREQ(result, tesuto2);
}

TEST_F(TestGenericWide, TestSectionUKeyUValA) {
  const wchar_t kensa[] = L"検査";
  const wchar_t tesuto[] = L"テスト";
  const wchar_t *result = ini.GetValue(kensa, tesuto);
  ASSERT_STREQ(result, L"test");
}

TEST_F(TestGenericWide, TestSectionUKeyUValU) {
  const wchar_t kensa[] = L"検査";
  const wchar_t tesuto2[] = L"テスト2";
  const wchar_t tesutoni[] = L"テスト二";
  const wchar_t *result = ini.GetValue(kensa, tesuto2);
  ASSERT_STREQ(result, tesutoni);
}
#endif // _WIN32
