#include "pch.h"
#include "../SimpleIni.h"

class TestWide : public ::testing::Test {
protected:
	void TestWide::SetUp() override;
protected:
	CSimpleIniW ini;
};

void TestWide::SetUp() {
	ini.SetUnicode();
	SI_Error err = ini.LoadFile(L"tests.ini");
	ASSERT_EQ(err, SI_OK);
}

TEST_F(TestWide, TestSectionAKeyAValA) {
	const wchar_t* result = ini.GetValue(L"section1", L"key1");
	ASSERT_STREQ(result, L"value1");
}

TEST_F(TestWide, TestSectionAKeyAValU) {
	const wchar_t tesuto2[] = L"テスト2";
	const wchar_t* result = ini.GetValue(L"section2", L"test2");
	ASSERT_STREQ(result, tesuto2);
}

TEST_F(TestWide, TestSectionAKeyUValA) {
	const wchar_t tesuto[] = L"テスト";
	const wchar_t* result = ini.GetValue(L"section2", tesuto);
	ASSERT_STREQ(result, L"test");
}

TEST_F(TestWide, TestSectionAKeyUValU) {
	const wchar_t tesuto2[] = L"テスト2";
	const wchar_t tesutoni[] = L"テスト二";
	const wchar_t* result = ini.GetValue(L"section2", tesuto2);
	ASSERT_STREQ(result, tesutoni);
}

TEST_F(TestWide, TestSectionUKeyAValA) {
	const wchar_t kensa[] = L"検査";
	const wchar_t* result = ini.GetValue(kensa, L"key2");
	ASSERT_STREQ(result, L"value2");
}

TEST_F(TestWide, TestSectionUKeyAValU) {
	const wchar_t kensa[] = L"検査";
	const wchar_t tesuto2[] = L"テスト2";
	const wchar_t* result = ini.GetValue(kensa, L"test2");
	ASSERT_STREQ(result, tesuto2);
}

TEST_F(TestWide, TestSectionUKeyUValA) {
	const wchar_t kensa[] = L"検査";
	const wchar_t tesuto[] = L"テスト";
	const wchar_t* result = ini.GetValue(kensa, tesuto);
	ASSERT_STREQ(result, L"test");
}

TEST_F(TestWide, TestSectionUKeyUValU) {
	const wchar_t kensa[] = L"検査";
	const wchar_t tesuto2[] = L"テスト2";
	const wchar_t tesutoni[] = L"テスト二";
	const wchar_t* result = ini.GetValue(kensa, tesuto2);
	ASSERT_STREQ(result, tesutoni);
}
