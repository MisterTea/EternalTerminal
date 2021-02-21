#include "pch.h"
#include "../SimpleIni.h"

TEST(TestBugFix, TestEmptySection) {
	CSimpleIniA ini;
	ini.SetValue("foo", "skey", "sval");
	ini.SetValue("", "rkey", "rval");
	ini.SetValue("bar", "skey", "sval");

	std::string output;
	ini.Save(output);

	std::string expected =
		"rkey = rval\n"
		"\n"
		"\n"
		"[foo]\n"
		"skey = sval\n"
		"\n"
		"\n"
		"[bar]\n"
		"skey = sval\n";

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(expected.c_str(), output.c_str());
}

TEST(TestBugFix, TestMultiLineIgnoreTrailSpace0) {
	std::string input =
		"; multiline values\n"
		"key = <<<EOS\n"
		"This is a\n"
		"multiline value\n"
		"and it ends.\n"
		"EOS\n"
		"\n"
		"[section]\n";

	bool multiline = true;
	CSimpleIniA ini(true, false, multiline);

	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	std::string output;
	ini.Save(output);

	std::string expected =
		"; multiline values\n"
		"\n"
		"\n"
		"key = <<<END_OF_TEXT\n"
		"This is a\n"
		"multiline value\n"
		"and it ends.\n"
		"END_OF_TEXT\n"
		"\n"
		"\n"
		"[section]\n";

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(expected.c_str(), output.c_str());
}

TEST(TestBugFix, TestMultiLineIgnoreTrailSpace1) {
	std::string input =
		"; multiline values\n"
		"key = <<<EOS\n"
		"This is a\n"
		"multiline value\n"
		"and it ends.\n"
		"EOS \n"
		"\n"
		"[section]\n";

	bool multiline = true;
	CSimpleIniA ini(true, false, multiline);

	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	std::string output;
	ini.Save(output);

	std::string expected =
		"; multiline values\n"
		"\n"
		"\n"
		"key = <<<END_OF_TEXT\n"
		"This is a\n"
		"multiline value\n"
		"and it ends.\n"
		"END_OF_TEXT\n"
		"\n"
		"\n"
		"[section]\n";

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(expected.c_str(), output.c_str());
}

TEST(TestBugFix, TestMultiLineIgnoreTrailSpace2) {
	std::string input =
		"; multiline values\n"
		"key = <<<EOS\n"
		"This is a\n"
		"multiline value\n"
		"and it ends.\n"
		"EOS  \n"
		"\n"
		"[section]\n";

	bool multiline = true;
	CSimpleIniA ini(true, false, multiline);

	SI_Error rc = ini.LoadData(input);
	ASSERT_EQ(rc, SI_OK);

	std::string output;
	ini.Save(output);

	std::string expected =
		"; multiline values\n"
		"\n"
		"\n"
		"key = <<<END_OF_TEXT\n"
		"This is a\n"
		"multiline value\n"
		"and it ends.\n"
		"END_OF_TEXT\n"
		"\n"
		"\n"
		"[section]\n";

	output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
	ASSERT_STREQ(expected.c_str(), output.c_str());
}
