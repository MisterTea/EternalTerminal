#include "pch.h"
#include <algorithm>
#include "../SimpleIni.h"

class TestRoundTrip : public ::testing::Test {
protected:
	void SetUp() override;
	void TestMulti();
	void TestBOM(bool useBOM);

protected:
	CSimpleIniA ini;
	std::string input;
	std::string output;
};

void TestRoundTrip::SetUp() {
	ini.SetUnicode();
}

TEST_F(TestRoundTrip, TestStandard) {
	input =
		"; File comment\n"
		"\n"
		"\n"
		"; Section 1 comment\n"
		"[section1]\n"
		"\n"
		"\n"
		"; Section 2 comment\n"
		"[section2]\n"
		"\n"
		"; key1 comment\n"
		"key1 = string\n"
		"\n"
		"; key 2 comment\n"
		"key2 = true\n"
		"key3 = 3.1415\n"
		;

	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	const char* result = ini.GetValue("section2", "key1");
	ASSERT_STREQ(result, "string");

	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(input.c_str(), output.c_str());
}

void TestRoundTrip::TestMulti() {
	input =
		"[section]\n"
		"key = string1\n"
		"key = string2\n"
		;

	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
}

TEST_F(TestRoundTrip, TestMultiGood) {
	ini.SetMultiKey(true);
	TestMulti();
	ASSERT_STREQ(input.c_str(), output.c_str());
}

TEST_F(TestRoundTrip, TestMultiBad) {
	std::string expected =
		"[section]\n"
		"key = string2\n";

	ini.SetMultiKey(false);
	TestMulti();
	ASSERT_STRNE(input.c_str(), output.c_str());
	ASSERT_STREQ(expected.c_str(), output.c_str());
}

TEST_F(TestRoundTrip, TestSpacesTrue) {
	input =
		"[section]\n"
		"key = string1\n";

	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	ini.SetSpaces(true);
	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	
	ASSERT_STREQ(input.c_str(), output.c_str());
}

TEST_F(TestRoundTrip, TestSpacesFalse) {
	input =
		"[section]\n"
		"key = string1\n";

	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	ini.SetSpaces(false);
	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());

	ASSERT_STRNE(input.c_str(), output.c_str());

	std::string expected =
		"[section]\n"
		"key=string1\n";

	ASSERT_STREQ(expected.c_str(), output.c_str());
}

void TestRoundTrip::TestBOM(bool useBOM) {
	const char bom[] = "\xEF\xBB\xBF";
	const char input8[] =
		u8"[テスト1]\n"
		u8"テスト2 = テスト3\n";

	input = bom;
	input += input8;

	ini.Reset();
	ini.SetUnicode(false);
	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	const char tesuto1[] = u8"テスト1";
	const char tesuto2[] = u8"テスト2";
	const char tesuto3[] = u8"テスト3";

	const char* result = ini.GetValue(tesuto1, tesuto2);
	ASSERT_STREQ(result, tesuto3);

	rc = ini.Save(output, useBOM);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
}

TEST_F(TestRoundTrip, TestWithBOM) {
	TestBOM(true);

	ASSERT_STREQ(input.c_str(), output.c_str());
}

TEST_F(TestRoundTrip, TestWithoutBOM) {
	TestBOM(false);

	ASSERT_STRNE(input.c_str(), output.c_str());

	std::string expected(input, 3);
	ASSERT_STREQ(expected.c_str(), output.c_str());
}

TEST_F(TestRoundTrip, TestAllowKeyOnly1) {
	ini.SetAllowKeyOnly(false);

	input =
		"[section1]\n"
		"key1 = string\n"
		"key2 = \n"
		"key3= \n"
		"key4=\n"
		"key5\n"
		"\n"
		"Never going to give you up\n"
		"Never going to let you down\n"
		;

	std::string expect =
		"[section1]\n"
		"key1 = string\n"
		"key2 = \n"
		"key3 = \n"
		"key4 = \n"
		;

	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(expect.c_str(), output.c_str());
}

TEST_F(TestRoundTrip, TestAllowKeyOnly2) {
	ini.SetAllowKeyOnly(true);

	input =
		"[section1]\n"
		"key1\n"
		"key2\n"
		"[section2]\n"
		"key1 = string\n"
		"key2 = \n"
		"key3= \n"
		"key4=\n"
		"\n"
		"key5\n"
		"\n"
		"Never going to give you up\n"
		"\n"
		"Never going to let you down\n"
		;

	std::string expect =
		"[section1]\n"
		"key1\n"
		"key2\n"
		"\n\n"
		"[section2]\n"
		"key1 = string\n"
		"key2\n"
		"key3\n"
		"key4\n"
		"key5\n"
		"Never going to give you up\n"
		"Never going to let you down\n"
		;

	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(expect.c_str(), output.c_str());
}

