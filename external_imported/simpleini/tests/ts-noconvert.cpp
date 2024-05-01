#include "pch.h"

#define SI_NO_CONVERSION
#include "../SimpleIni.h"

class TestNoConvert : public ::testing::Test {
protected:
	void SetUp() override;
protected:
	CSimpleIniA ini;
};

void TestNoConvert::SetUp() {
	ini.SetUnicode();
	SI_Error err = ini.LoadFile("tests.ini");
	ASSERT_EQ(err, SI_OK);
}

TEST_F(TestNoConvert, TestSectionAKeyAValA) {
	const char* result = ini.GetValue("section1", "key1");
	ASSERT_STREQ(result, "value1");
}

TEST_F(TestNoConvert, TestSectionAKeyAValU) {
	const char tesuto2[] = u8"テスト2"; 
	const char* result = ini.GetValue("section2", "test2");
	ASSERT_STREQ(result, tesuto2);
}

TEST_F(TestNoConvert, TestSectionAKeyUValA) {
	const char tesuto[] = u8"テスト";
	const char* result = ini.GetValue("section2", tesuto);
	ASSERT_STREQ(result, "test");
}

TEST_F(TestNoConvert, TestSectionAKeyUValU) {
	const char tesuto2[] = u8"テスト2";
	const char tesutoni[] = u8"テスト二";
	const char* result = ini.GetValue("section2", tesuto2);
	ASSERT_STREQ(result, tesutoni);
}

TEST_F(TestNoConvert, TestSectionUKeyAValA) {
	const char kensa[] = u8"検査";
	const char* result = ini.GetValue(kensa, "key2");
	ASSERT_STREQ(result, "value2");
}

TEST_F(TestNoConvert, TestSectionUKeyAValU) {
	const char kensa[] = u8"検査";
	const char tesuto2[] = u8"テスト2";
	const char* result = ini.GetValue(kensa, "test2");
	ASSERT_STREQ(result, tesuto2);
}

TEST_F(TestNoConvert, TestSectionUKeyUValA) {
	const char kensa[] = u8"検査";
	const char tesuto[] = u8"テスト";
	const char* result = ini.GetValue(kensa, tesuto);
	ASSERT_STREQ(result, "test");
}

TEST_F(TestNoConvert, TestSectionUKeyUValU) {
	const char kensa[] = u8"検査";
	const char tesuto2[] = u8"テスト2";
	const char tesutoni[] = u8"テスト二";
	const char* result = ini.GetValue(kensa, tesuto2);
	ASSERT_STREQ(result, tesutoni);
}
