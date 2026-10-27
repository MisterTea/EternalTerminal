#include "../SimpleIni.h"
#include "gtest/gtest.h"
#include <climits>
#include <cmath>
#include <limits>

class TestNumeric : public ::testing::Test {
protected:
  void SetUp() override;

protected:
  CSimpleIniA ini;
};

void TestNumeric::SetUp() { ini.SetUnicode(); }

// Test GetLongValue with valid integers
TEST_F(TestNumeric, TestGetLongValuePositive) {
  std::string input = "[numbers]\n"
                      "positive = 42\n"
                      "zero = 0\n"
                      "negative = -123\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  long result = ini.GetLongValue("numbers", "positive", 0);
  ASSERT_EQ(result, 42);

  result = ini.GetLongValue("numbers", "zero", -1);
  ASSERT_EQ(result, 0);

  result = ini.GetLongValue("numbers", "negative", 0);
  ASSERT_EQ(result, -123);
}

// Test GetLongValue with hex values
TEST_F(TestNumeric, TestGetLongValueHex) {
  std::string input = "[numbers]\n"
                      "hex1 = 0xFF\n"
                      "hex2 = 0x10\n"
                      "hex3 = 0x12345678\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  long result = ini.GetLongValue("numbers", "hex1", 0);
  ASSERT_EQ(result, 0xFF);

  result = ini.GetLongValue("numbers", "hex2", 0);
  ASSERT_EQ(result, 0x10);

  result = ini.GetLongValue("numbers", "hex3", 0);
  ASSERT_EQ(result, 0x12345678);
}

// Test GetLongValue with invalid values (should return default)
TEST_F(TestNumeric, TestGetLongValueInvalid) {
  std::string input = "[numbers]\n"
                      "text = hello\n"
                      "empty = \n"
                      "partial = 123abc\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  long result = ini.GetLongValue("numbers", "text", 999);
  ASSERT_EQ(result, 999);

  // Empty string returns default (SimpleIni behavior, not raw strtol)
  result = ini.GetLongValue("numbers", "empty", 999);
  ASSERT_EQ(result, 999);

  // "123abc" returns default - SimpleIni validates the full string
  result = ini.GetLongValue("numbers", "partial", 999);
  ASSERT_EQ(result, 999);
}

// Test GetLongValue with non-existent key
TEST_F(TestNumeric, TestGetLongValueMissing) {
  std::string input = "[numbers]\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  long result = ini.GetLongValue("numbers", "missing", 777);
  ASSERT_EQ(result, 777);

  result = ini.GetLongValue("missing_section", "key", 888);
  ASSERT_EQ(result, 888);
}

// Test SetLongValue
TEST_F(TestNumeric, TestSetLongValue) {
  SI_Error rc = ini.SetLongValue("numbers", "value1", 12345);
  ASSERT_EQ(rc, SI_INSERTED);

  long result = ini.GetLongValue("numbers", "value1", 0);
  ASSERT_EQ(result, 12345);

  // Update existing value
  rc = ini.SetLongValue("numbers", "value1", 67890);
  ASSERT_EQ(rc, SI_UPDATED);

  result = ini.GetLongValue("numbers", "value1", 0);
  ASSERT_EQ(result, 67890);
}

// Test SetLongValue with hex format
TEST_F(TestNumeric, TestSetLongValueHex) {
  SI_Error rc = ini.SetLongValue("numbers", "hexval", 255, nullptr, true);
  ASSERT_EQ(rc, SI_INSERTED);

  std::string output;
  rc = ini.Save(output);
  ASSERT_EQ(rc, SI_OK);

  // Should be written as hex
  EXPECT_NE(output.find("0xff"), std::string::npos);

  // Should read back correctly
  long result = ini.GetLongValue("numbers", "hexval", 0);
  ASSERT_EQ(result, 255);
}

// Test SetLongValue with negative values
TEST_F(TestNumeric, TestSetLongValueNegative) {
  SI_Error rc = ini.SetLongValue("numbers", "negative", -9999);
  ASSERT_EQ(rc, SI_INSERTED);

  long result = ini.GetLongValue("numbers", "negative", 0);
  ASSERT_EQ(result, -9999);
}

// Test GetDoubleValue with valid doubles
TEST_F(TestNumeric, TestGetDoubleValue) {
  std::string input = "[floats]\n"
                      "pi = 3.14159\n"
                      "negative = -2.5\n"
                      "integer = 42.0\n"
                      "scientific = 1.23e-4\n"
                      "zero = 0.0\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  double result = ini.GetDoubleValue("floats", "pi", 0.0);
  ASSERT_NEAR(result, 3.14159, 0.00001);

  result = ini.GetDoubleValue("floats", "negative", 0.0);
  ASSERT_NEAR(result, -2.5, 0.00001);

  result = ini.GetDoubleValue("floats", "integer", 0.0);
  ASSERT_NEAR(result, 42.0, 0.00001);

  result = ini.GetDoubleValue("floats", "scientific", 0.0);
  ASSERT_NEAR(result, 1.23e-4, 0.000001);

  result = ini.GetDoubleValue("floats", "zero", 1.0);
  ASSERT_NEAR(result, 0.0, 0.00001);
}

// Test GetDoubleValue with invalid values
TEST_F(TestNumeric, TestGetDoubleValueInvalid) {
  std::string input = "[floats]\n"
                      "text = not_a_number\n"
                      "empty = \n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  double result = ini.GetDoubleValue("floats", "text", 99.9);
  ASSERT_NEAR(result, 99.9, 0.00001);

  result = ini.GetDoubleValue("floats", "empty", 88.8);
  ASSERT_NEAR(result, 88.8, 0.00001);
}

// Test SetDoubleValue
TEST_F(TestNumeric, TestSetDoubleValue) {
  SI_Error rc = ini.SetDoubleValue("floats", "value1", 3.14159);
  ASSERT_EQ(rc, SI_INSERTED);

  double result = ini.GetDoubleValue("floats", "value1", 0.0);
  ASSERT_NEAR(result, 3.14159, 0.00001);

  // Update existing value
  rc = ini.SetDoubleValue("floats", "value1", 2.71828);
  ASSERT_EQ(rc, SI_UPDATED);

  result = ini.GetDoubleValue("floats", "value1", 0.0);
  ASSERT_NEAR(result, 2.71828, 0.00001);
}

// Test SetDoubleValue with negative and scientific notation
TEST_F(TestNumeric, TestSetDoubleValueFormats) {
  SI_Error rc = ini.SetDoubleValue("floats", "negative", -123.456);
  ASSERT_EQ(rc, SI_INSERTED);

  double result = ini.GetDoubleValue("floats", "negative", 0.0);
  ASSERT_NEAR(result, -123.456, 0.0001);

  rc = ini.SetDoubleValue("floats", "tiny", 0.000001);
  ASSERT_EQ(rc, SI_INSERTED);

  result = ini.GetDoubleValue("floats", "tiny", 0.0);
  ASSERT_NEAR(result, 0.000001, 0.0000001);
}

// Test multikey with numeric values
TEST_F(TestNumeric, TestMultikeyNumeric) {
  ini.SetMultiKey(true);

  std::string input = "[numbers]\n"
                      "value = 10\n"
                      "value = 20\n"
                      "value = 30\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  bool hasMultiple = false;
  long result = ini.GetLongValue("numbers", "value", 0, &hasMultiple);
  ASSERT_EQ(result, 10);
  ASSERT_TRUE(hasMultiple);

  // Get all values
  CSimpleIniA::TNamesDepend values;
  ini.GetAllValues("numbers", "value", values);
  ASSERT_EQ(values.size(), 3);
}

// Test SetLongValue with force replace in multikey mode
TEST_F(TestNumeric, TestSetLongValueForceReplace) {
  ini.SetMultiKey(true);

  std::string input = "[numbers]\n"
                      "value = 10\n"
                      "value = 20\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  // Replace all values
  rc = ini.SetLongValue("numbers", "value", 999, nullptr, false, true);
  ASSERT_EQ(rc, SI_UPDATED);

  // Should only have one value now
  bool hasMultiple = false;
  long result = ini.GetLongValue("numbers", "value", 0, &hasMultiple);
  ASSERT_EQ(result, 999);
  ASSERT_FALSE(hasMultiple);
}

// Test extreme values
TEST_F(TestNumeric, TestExtremeValues) {
  SI_Error rc = ini.SetLongValue("numbers", "max", LONG_MAX);
  ASSERT_EQ(rc, SI_INSERTED);

  rc = ini.SetLongValue("numbers", "min", LONG_MIN);
  ASSERT_EQ(rc, SI_INSERTED);

  long result = ini.GetLongValue("numbers", "max", 0);
  ASSERT_EQ(result, LONG_MAX);

  result = ini.GetLongValue("numbers", "min", 0);
  ASSERT_EQ(result, LONG_MIN);
}

// Test numeric values with whitespace
TEST_F(TestNumeric, TestNumericWhitespace) {
  std::string input = "[numbers]\n"
                      "padded =   42   \n"
                      "tabs =\t123\t\n";

  SI_Error rc = ini.LoadData(input);
  ASSERT_EQ(rc, SI_OK);

  long result = ini.GetLongValue("numbers", "padded", 0);
  ASSERT_EQ(result, 42);

  result = ini.GetLongValue("numbers", "tabs", 0);
  ASSERT_EQ(result, 123);
}

// Test numeric roundtrip
TEST_F(TestNumeric, TestNumericRoundtrip) {
  ini.SetLongValue("test", "long1", 12345);
  ini.SetLongValue("test", "long2", -67890);
  ini.SetDoubleValue("test", "double1", 3.14159);
  ini.SetDoubleValue("test", "double2", -2.71828);

  std::string output;
  SI_Error rc = ini.Save(output);
  ASSERT_EQ(rc, SI_OK);

  // Load it back
  CSimpleIniA ini2;
  ini2.SetUnicode();
  rc = ini2.LoadData(output);
  ASSERT_EQ(rc, SI_OK);

  ASSERT_EQ(ini2.GetLongValue("test", "long1", 0), 12345);
  ASSERT_EQ(ini2.GetLongValue("test", "long2", 0), -67890);
  ASSERT_NEAR(ini2.GetDoubleValue("test", "double1", 0.0), 3.14159, 0.00001);
  ASSERT_NEAR(ini2.GetDoubleValue("test", "double2", 0.0), -2.71828, 0.00001);
}
