#include "pch.h"
#include "../SimpleIni.h"


// ### SIMPLE USAGE

TEST(TestSnippets, TestSimple) {
	// simple demonstration

	CSimpleIniA ini;
	ini.SetUnicode();

	SI_Error rc = ini.LoadFile("example.ini");
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_OK);

	const char* pv;
	pv = ini.GetValue("section", "key", "default");
	ASSERT_STREQ(pv, "value");

	ini.SetValue("section", "key", "newvalue");

	pv = ini.GetValue("section", "key", "default");
	ASSERT_STREQ(pv, "newvalue");
}


// ### LOADING DATA

TEST(TestSnippets, TestLoadFile) {
	// load from a data file
	CSimpleIniA ini;
	SI_Error rc = ini.LoadFile("example.ini");
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_OK);
}

TEST(TestSnippets, TestLoadString) {
	// load from a string
	const std::string example = "[section]\nkey = value\n";
	CSimpleIniA ini;
	SI_Error rc = ini.LoadData(example);
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_OK);
}


// ### GETTING SECTIONS AND KEYS

TEST(TestSnippets, TestSectionsAndKeys) {
	const std::string example =
		"[section1]\n"
		"key1 = value1\n"
		"key2 = value2\n"
		"\n"
		"[section2]\n"
		"[section3]\n";

	CSimpleIniA ini;
	SI_Error rc = ini.LoadData(example);
	ASSERT_EQ(rc, SI_OK);
	


	// get all sections
	CSimpleIniA::TNamesDepend sections;
	ini.GetAllSections(sections);

	// get all keys in a section
	CSimpleIniA::TNamesDepend keys;
	ini.GetAllKeys("section1", keys);



	const char* expectedSections[] = { "section1", "section2", "section3", nullptr };
	const char* expectedKeys[] = { "key1", "key2", nullptr };

	CSimpleIniA::TNamesDepend::const_iterator it;
	int i;

	for (i = 0, it = sections.begin(); it != sections.end(); ++i, ++it) {
		ASSERT_NE(expectedSections[i], nullptr);
		ASSERT_STREQ(expectedSections[i], it->pItem);
	}
	ASSERT_EQ(expectedSections[i], nullptr);

	for (i = 0, it = keys.begin(); it != keys.end(); ++i, ++it) {
		ASSERT_NE(expectedKeys[i], nullptr);
		ASSERT_STREQ(expectedKeys[i], it->pItem);
	}
	ASSERT_EQ(expectedKeys[i], nullptr);
}


// ### GETTING VALUES

TEST(TestSnippets, TestGettingValues) {
	const std::string example =
		"[section1]\n"
		"key1 = value1\n"
		"key2 = value2.1\n"
		"key2 = value2.2\n"
		"\n"
		"[section2]\n"
		"[section3]\n";

	bool utf8 = true;
	bool multiKey = true;
	CSimpleIniA ini(utf8, multiKey);
	SI_Error rc = ini.LoadData(example);
	ASSERT_EQ(rc, SI_OK);


	// get the value of a key that doesn't exist
	const char* pv;
	pv = ini.GetValue("section1", "key99");
	ASSERT_EQ(pv, nullptr);

	// get the value of a key that does exist
	pv = ini.GetValue("section1", "key1");
	ASSERT_STREQ(pv, "value1");

	// get the value of a key which may have multiple 
	// values. If hasMultiple is true, then there are
	// multiple values and just one value has been returned
	bool hasMulti;
	pv = ini.GetValue("section1", "key1", nullptr, &hasMulti);
	ASSERT_STREQ(pv, "value1");
	ASSERT_EQ(hasMulti, false);

	pv = ini.GetValue("section1", "key2", nullptr, &hasMulti);
	ASSERT_STREQ(pv, "value2.1");
	ASSERT_EQ(hasMulti, true);

	// get all values of a key with multiple values
	CSimpleIniA::TNamesDepend values;
	ini.GetAllValues("section1", "key2", values);

	// sort the values into a known order, in this case we want
	// the original load order
	values.sort(CSimpleIniA::Entry::LoadOrder());

	// output all of the items
	CSimpleIniA::TNamesDepend::const_iterator it;
	for (it = values.begin(); it != values.end(); ++it) {
		//printf("value = '%s'\n", it->pItem);
	}


	int i;
	const char* expectedValues[] = { "value2.1", "value2.2", nullptr };
	for (i = 0, it = values.begin(); it != values.end(); ++it, ++i) {
		ASSERT_NE(expectedValues[i], nullptr);
		ASSERT_STREQ(expectedValues[i], it->pItem);
	}
	ASSERT_EQ(expectedValues[i], nullptr);
}

// ### VALUE EXISTS

TEST(TestSnippets, TestExists)
{
	const std::string example =
		"[section1]\n"
		"key1 = value1\n"
		"key2 = value2.1\n"
		"key2 = value2.2\n"
		"\n"
		"[section2]\n"
		"key1\n"
		"key2\n"
		"[section3]\n";

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.SetMultiKey();
	ini.SetAllowKeyOnly();

	SI_Error rc = ini.LoadData(example);
	ASSERT_EQ(rc, SI_OK);


	// check for section doesn't exist
	EXPECT_FALSE(ini.SectionExists(""));
	EXPECT_FALSE(ini.SectionExists("section4"));

	// check for section does exist
	EXPECT_TRUE(ini.SectionExists("section1"));
	EXPECT_TRUE(ini.SectionExists("section2"));
	EXPECT_TRUE(ini.SectionExists("section3"));

	// check for key doesn't exist
	EXPECT_FALSE(ini.KeyExists("", "key"));
	EXPECT_FALSE(ini.KeyExists("section1", "key"));
	EXPECT_FALSE(ini.KeyExists("section2", "key"));

	// check for key does exist
	EXPECT_TRUE(ini.KeyExists("section1", "key1"));
	EXPECT_TRUE(ini.KeyExists("section1", "key2"));
	EXPECT_TRUE(ini.KeyExists("section2", "key1"));
	EXPECT_TRUE(ini.KeyExists("section2", "key2"));
}

// ### MODIFYING DATA

TEST(TestSnippets, TestModifyingData) {
	bool utf8 = true;
	bool multiKey = false;
	CSimpleIniA ini(utf8, multiKey);
	SI_Error rc;


	// add a new section 
	rc = ini.SetValue("section1", nullptr, nullptr);
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_INSERTED); 

	// not an error to add one that already exists
	rc = ini.SetValue("section1", nullptr, nullptr);
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_UPDATED);

	// get the value of a key that doesn't exist
	const char* pv;
	pv = ini.GetValue("section2", "key1", "default-value");
	ASSERT_STREQ(pv, "default-value");

	// adding a key (the section will be added if needed)
	rc = ini.SetValue("section2", "key1", "value1");
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_INSERTED);

	// ensure it is set to expected value
	pv = ini.GetValue("section2", "key1", nullptr);
	ASSERT_STREQ(pv, "value1");

	// change the value of a key
	rc = ini.SetValue("section2", "key1", "value2");
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_UPDATED);

	// ensure it is set to expected value
	pv = ini.GetValue("section2", "key1", nullptr);
	ASSERT_STREQ(pv, "value2");
}


// ### DELETING DATA

TEST(TestSnippets, TestDeletingData) {
	const std::string example =
		"[section1]\n"
		"key1 = value1\n"
		"key2 = value2\n"
		"\n"
		"[section2]\n"
		"key1 = value1\n"
		"key2 = value2\n"
		"\n"
		"[section3]\n";

	bool utf8 = true;
	CSimpleIniA ini(utf8);
	SI_Error rc = ini.LoadData(example);
	ASSERT_EQ(rc, SI_OK);


	// deleting a key from a section. Optionally the entire
	// section may be deleted if it is now empty.
	bool done, deleteSectionIfEmpty = true;
	done = ini.Delete("section1", "key1", deleteSectionIfEmpty);
	ASSERT_EQ(done, true);
	done = ini.Delete("section1", "key1");
	ASSERT_EQ(done, false);

	// deleting an entire section and all keys in it
	done = ini.Delete("section2", nullptr);
	ASSERT_EQ(done, true);
	done = ini.Delete("section2", nullptr);
	ASSERT_EQ(done, false);
}


// ### SAVING DATA

TEST(TestSnippets, TestSavingData) {
	bool utf8 = true;
	CSimpleIniA ini(utf8);
	SI_Error rc;


	// save the data to a string
	std::string data;
	rc = ini.Save(data);
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_OK);

	// save the data back to the file
	rc = ini.SaveFile("example2.ini");
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_OK);
}

