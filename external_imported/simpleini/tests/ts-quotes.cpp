#include "pch.h"
#include <algorithm>
#include "../SimpleIni.h"

class TestQuotes : public ::testing::Test {
protected:
	void SetUp() override;

protected:
	CSimpleIniA ini;
	std::string input;
	std::string expect;
	std::string output;
};

void TestQuotes::SetUp() {
	ini.SetUnicode();
}

TEST_F(TestQuotes, TestEmpty) {
	ini.SetQuotes(true);

	input =
		"[section]\n"
		"key1 = \"\"\n"
		"key2 = \n"
		;

	// no need to preserve quotes for empty data
	expect =
		"[section]\n"
		"key1 = \n"
		"key2 = \n"
		;

	const char* result;
	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	result = ini.GetValue("section", "key1");
	ASSERT_STREQ(result, "");

	result = ini.GetValue("section", "key2");
	ASSERT_STREQ(result, "");

	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(expect.c_str(), output.c_str());
}

TEST_F(TestQuotes, TestEmptyDisabled) {
	ini.SetQuotes(false);

	input =
		"[section]\n"
		"key1 = \"\"\n"
		"key2 = \n"
		;

	const char* result;
	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	result = ini.GetValue("section", "key1");
	ASSERT_STREQ(result, "\"\"");

	result = ini.GetValue("section", "key2");
	ASSERT_STREQ(result, "");

	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(input.c_str(), output.c_str());
}

TEST_F(TestQuotes, TestGeneral) {
	ini.SetQuotes(true);

	input =
		"[section]\n"
		"key1 = foo\n"
		"key2 = \"foo\"\n"
		"key3 =  foo \n"
		"key4 = \" foo \"\n"
		"key5 = \"foo\n"
		"key6 = foo\"\n"
		"key7 =  foo \" foo \n"
		"key8 =  \" foo \" foo \" \n"
		;

	expect =
		"[section]\n"
		"key1 = foo\n"
		"key2 = foo\n"
		"key3 = foo\n"
		"key4 = \" foo \"\n"
		"key5 = \"foo\n"
		"key6 = foo\"\n"
		"key7 = foo \" foo\n"
		"key8 = \" foo \" foo \"\n"
		;

	const char* result;
	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	result = ini.GetValue("section", "key1");
	ASSERT_STREQ(result, "foo");

	result = ini.GetValue("section", "key2");
	ASSERT_STREQ(result, "foo");

	result = ini.GetValue("section", "key3");
	ASSERT_STREQ(result, "foo");

	result = ini.GetValue("section", "key4");
	ASSERT_STREQ(result, " foo ");

	result = ini.GetValue("section", "key5");
	ASSERT_STREQ(result, "\"foo");

	result = ini.GetValue("section", "key6");
	ASSERT_STREQ(result, "foo\"");

	result = ini.GetValue("section", "key7");
	ASSERT_STREQ(result, "foo \" foo");

	result = ini.GetValue("section", "key8");
	ASSERT_STREQ(result, " foo \" foo ");

	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(expect.c_str(), output.c_str());
}

TEST_F(TestQuotes, TestGeneralDisabled) {
	ini.SetQuotes(false);

	input =
		"[section]\n"
		"key1 = foo\n"
		"key2 = \"foo\"\n"
		"key3 =  foo \n"
		"key4 = \" foo \"\n"
		"key5 = \"foo\n"
		"key6 = foo\"\n"
		"key7 =  foo \" foo \n"
		"key8 =  \" foo \" foo \" \n"
		;

	expect =
		"[section]\n"
		"key1 = foo\n"
		"key2 = \"foo\"\n"
		"key3 = foo\n"
		"key4 = \" foo \"\n"
		"key5 = \"foo\n"
		"key6 = foo\"\n"
		"key7 = foo \" foo\n"
		"key8 = \" foo \" foo \"\n"
		;

	const char* result;
	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	rc = ini.Save(output);
	ASSERT_EQ(rc, SI_OK);

	result = ini.GetValue("section", "key1");
	ASSERT_STREQ(result, "foo");

	result = ini.GetValue("section", "key2");
	ASSERT_STREQ(result, "\"foo\"");

	result = ini.GetValue("section", "key3");
	ASSERT_STREQ(result, "foo");

	result = ini.GetValue("section", "key4");
	ASSERT_STREQ(result, "\" foo \"");

	result = ini.GetValue("section", "key5");
	ASSERT_STREQ(result, "\"foo");

	result = ini.GetValue("section", "key6");
	ASSERT_STREQ(result, "foo\"");

	result = ini.GetValue("section", "key7");
	ASSERT_STREQ(result, "foo \" foo");

	result = ini.GetValue("section", "key8");
	ASSERT_STREQ(result, "\" foo \" foo \"");

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(expect.c_str(), output.c_str());
}

