#include "../SimpleIni.h"
#include "gtest/gtest.h"

class TestSections : public ::testing::Test {
protected:
  void SetUp() override;

protected:
  CSimpleIniA ini;
};

void TestSections::SetUp() { ini.SetUnicode(); }

// Test GetSectionSize
TEST_F(TestSections, TestGetSectionSize) {
  std::string input = "[section1]\n"
                      "key1 = value1\n"
                      "key2 = value2\n"
                      "key3 = value3\n"
                      "\n"
                      "[section2]\n"
                      "key1 = value1\n"
                      "\n"
                      "[empty]\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  int size = ini.GetSectionSize("section1");
  ASSERT_EQ(size, 3);

  size = ini.GetSectionSize("section2");
  ASSERT_EQ(size, 1);

  size = ini.GetSectionSize("empty");
  ASSERT_EQ(size, 0);

  // Non-existent section
  size = ini.GetSectionSize("missing");
  ASSERT_EQ(size, -1);
}

// Test GetSectionSize with multikey
TEST_F(TestSections, TestGetSectionSizeMultikey) {
  ini.SetMultiKey(true);

  std::string input = "[section]\n"
                      "key1 = value1\n"
                      "key1 = value2\n"
                      "key2 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Should count unique keys, not total entries
  int size = ini.GetSectionSize("section");
  ASSERT_EQ(size, 2);
}

// Test GetSection
TEST_F(TestSections, TestGetSection) {
  std::string input = "[section1]\n"
                      "key1 = value1\n"
                      "key2 = value2\n"
                      "key3 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const CSimpleIniA::TKeyVal *section = ini.GetSection("section1");
  ASSERT_NE(section, nullptr);
  ASSERT_EQ(section->size(), 3);

  // Verify we can iterate the section
  int count = 0;
  for (auto it = section->begin(); it != section->end(); ++it) {
    count++;
    ASSERT_NE(it->first.pItem, nullptr);
    ASSERT_NE(it->second, nullptr);
  }
  ASSERT_EQ(count, 3);
}

// Test GetSection returns nullptr for missing section
TEST_F(TestSections, TestGetSectionMissing) {
  std::string input = "[section1]\nkey=value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const CSimpleIniA::TKeyVal *section = ini.GetSection("missing");
  ASSERT_EQ(section, nullptr);
}

// Test SectionExists
TEST_F(TestSections, TestSectionExists) {
  std::string input = "[section1]\n"
                      "key = value\n"
                      "\n"
                      "[section2]\n"
                      "\n"
                      "[empty]\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.SectionExists("section1"));
  ASSERT_TRUE(ini.SectionExists("section2"));
  ASSERT_TRUE(ini.SectionExists("empty"));
  ASSERT_FALSE(ini.SectionExists("missing"));
  ASSERT_FALSE(ini.SectionExists(""));
}

// Test KeyExists
TEST_F(TestSections, TestKeyExists) {
  std::string input = "[section1]\n"
                      "key1 = value1\n"
                      "key2 = value2\n"
                      "\n"
                      "[section2]\n"
                      "key3 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.KeyExists("section1", "key1"));
  ASSERT_TRUE(ini.KeyExists("section1", "key2"));
  ASSERT_FALSE(ini.KeyExists("section1", "key3"));
  ASSERT_FALSE(ini.KeyExists("section1", "missing"));

  ASSERT_TRUE(ini.KeyExists("section2", "key3"));
  ASSERT_FALSE(ini.KeyExists("section2", "key1"));

  ASSERT_FALSE(ini.KeyExists("missing", "key"));
}

// Test KeyExists with empty section
TEST_F(TestSections, TestKeyExistsEmptySection) {
  ini.SetAllowKeyOnly(true);

  std::string input = "key1 = value1\n"
                      "[section]\n"
                      "key2 = value2\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_TRUE(ini.KeyExists("", "key1"));
  ASSERT_FALSE(ini.KeyExists("", "key2"));
  ASSERT_TRUE(ini.KeyExists("section", "key2"));
}

// Test GetAllSections
TEST_F(TestSections, TestGetAllSections) {
  std::string input = "[section1]\n"
                      "key = value\n"
                      "\n"
                      "[section2]\n"
                      "key = value\n"
                      "\n"
                      "[section3]\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  CSimpleIniA::TNamesDepend sections;
  ini.GetAllSections(sections);

  ASSERT_EQ(sections.size(), 3);

  // Verify section names
  bool found1 = false, found2 = false, found3 = false;
  for (auto it = sections.begin(); it != sections.end(); ++it) {
    if (strcmp(it->pItem, "section1") == 0)
      found1 = true;
    if (strcmp(it->pItem, "section2") == 0)
      found2 = true;
    if (strcmp(it->pItem, "section3") == 0)
      found3 = true;
  }
  ASSERT_TRUE(found1);
  ASSERT_TRUE(found2);
  ASSERT_TRUE(found3);
}

// Test GetAllKeys
TEST_F(TestSections, TestGetAllKeys) {
  std::string input = "[section1]\n"
                      "key1 = value1\n"
                      "key2 = value2\n"
                      "key3 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  CSimpleIniA::TNamesDepend keys;
  bool result = ini.GetAllKeys("section1", keys);
  ASSERT_TRUE(result);
  ASSERT_EQ(keys.size(), 3);

  // Verify key names
  bool found1 = false, found2 = false, found3 = false;
  for (auto it = keys.begin(); it != keys.end(); ++it) {
    if (strcmp(it->pItem, "key1") == 0)
      found1 = true;
    if (strcmp(it->pItem, "key2") == 0)
      found2 = true;
    if (strcmp(it->pItem, "key3") == 0)
      found3 = true;
  }
  ASSERT_TRUE(found1);
  ASSERT_TRUE(found2);
  ASSERT_TRUE(found3);
}

// Test GetAllKeys for missing section
TEST_F(TestSections, TestGetAllKeysMissing) {
  std::string input = "[section1]\nkey=value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  CSimpleIniA::TNamesDepend keys;
  bool result = ini.GetAllKeys("missing", keys);
  ASSERT_FALSE(result);
  ASSERT_EQ(keys.size(), 0);
}

// Test GetAllKeys with multikey
TEST_F(TestSections, TestGetAllKeysMultikey) {
  ini.SetMultiKey(true);

  std::string input = "[section]\n"
                      "key1 = value1\n"
                      "key1 = value2\n"
                      "key2 = value3\n"
                      "key2 = value4\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  CSimpleIniA::TNamesDepend keys;
  bool result = ini.GetAllKeys("section", keys);
  ASSERT_TRUE(result);

  // Should return unique keys
  ASSERT_EQ(keys.size(), 2);
}

// Test creating empty section
TEST_F(TestSections, TestCreateEmptySection) {
  SI_Error rc = ini.SetValue("newsection", nullptr, nullptr);
  ASSERT_EQ(rc, SI_INSERTED);

  ASSERT_TRUE(ini.SectionExists("newsection"));
  ASSERT_EQ(ini.GetSectionSize("newsection"), 0);

  // Adding same section again should update, not insert
  rc = ini.SetValue("newsection", nullptr, nullptr);
  ASSERT_EQ(rc, SI_UPDATED);
}

// Test section with only keys (no equals sign)
TEST_F(TestSections, TestSectionWithKeyOnly) {
  ini.SetAllowKeyOnly(true);

  std::string input = "[section]\n"
                      "key1\n"
                      "key2\n"
                      "key3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  int size = ini.GetSectionSize("section");
  ASSERT_EQ(size, 3);

  ASSERT_TRUE(ini.KeyExists("section", "key1"));
  ASSERT_TRUE(ini.KeyExists("section", "key2"));
  ASSERT_TRUE(ini.KeyExists("section", "key3"));

  // Values should be NULL or empty
  const char *value = ini.GetValue("section", "key1");
  ASSERT_TRUE(value == nullptr || strlen(value) == 0);
}

// Test GetSection with multikey returns all entries
TEST_F(TestSections, TestGetSectionMultikey) {
  ini.SetMultiKey(true);

  std::string input = "[section]\n"
                      "key1 = value1\n"
                      "key1 = value2\n"
                      "key2 = value3\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  const CSimpleIniA::TKeyVal *section = ini.GetSection("section");
  ASSERT_NE(section, nullptr);

  // Should have 3 total entries (not 2)
  ASSERT_EQ(section->size(), 3);
}

// Test empty ini
TEST_F(TestSections, TestEmptyIni) {
  ASSERT_TRUE(ini.IsEmpty());

  CSimpleIniA::TNamesDepend sections;
  ini.GetAllSections(sections);
  ASSERT_EQ(sections.size(), 0);

  ASSERT_FALSE(ini.SectionExists("anything"));
  ASSERT_EQ(ini.GetSectionSize("anything"), -1);
  ASSERT_EQ(ini.GetSection("anything"), nullptr);
}

// Test Reset clears all data
TEST_F(TestSections, TestReset) {
  std::string input = "[section1]\n"
                      "key = value\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_FALSE(ini.IsEmpty());
  ASSERT_TRUE(ini.SectionExists("section1"));

  ini.Reset();

  ASSERT_TRUE(ini.IsEmpty());
  ASSERT_FALSE(ini.SectionExists("section1"));
}
