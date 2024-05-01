simpleini
=========

![Test Results Linux](https://github.com/brofield/simpleini/actions/workflows/test.yml/badge.svg)
![Test Results Windows](https://ci.appveyor.com/api/projects/status/github/brofield/simpleini?svg=true)

A cross-platform library that provides a simple API to read and write INI-style configuration files. It supports data files in ASCII, MBCS and Unicode. It is designed explicitly to be portable to any platform and has been tested on Windows, WinCE and Linux. Released as open-source and free using the MIT licence.

# Feature Summary

- MIT Licence allows free use in all software (including GPL and commercial)
- multi-platform (Windows 95 to Windows 10, Windows CE, Linux, Unix)
- loading and saving of INI-style configuration files
- configuration files can have any newline format on all platforms
- liberal acceptance of file format
  * key/values with no section, keys with no value
  * removal of whitespace around sections, keys and values
- support for multi-line values (values with embedded newline characters)
- optional support for multiple keys with the same name
- optional case-insensitive sections and keys (for ASCII characters only)
- saves files with sections and keys in the same order as they were loaded
- preserves comments on the file, section and keys where possible
- supports both char or wchar_t programming interfaces
- supports both MBCS (system locale) and UTF-8 file encodings
- system locale does not need to be UTF-8 on Linux/Unix to load UTF-8 file
- support for non-ASCII characters in section, keys, values and comments
- support for non-standard character types or file encodings via user-written converter classes
- support for adding/modifying values programmatically
- should compile with no warnings in most compilers

# Documentation

Full documentation of the interface is available in doxygen format.

# Examples

These snippets are included with the distribution in the automatic tests as ts-snippets.cpp.

### SIMPLE USAGE

```c++
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
```

### LOADING DATA

```c++
	// load from a data file
	CSimpleIniA ini;
	SI_Error rc = ini.LoadFile("example.ini");
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_OK);

	// load from a string
	const std::string example = "[section]\nkey = value\n";
	CSimpleIniA ini;
	SI_Error rc = ini.LoadData(example);
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_OK);
```

### GETTING SECTIONS AND KEYS

```c++
	// get all sections
	CSimpleIniA::TNamesDepend sections;
	ini.GetAllSections(sections);

	// get all keys in a section
	CSimpleIniA::TNamesDepend keys;
	ini.GetAllKeys("section1", keys);
```

### GETTING VALUES

```c++
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
		printf("value = '%s'\n", it->pItem);
	}
```

### MODIFYING DATA

```c++
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
```

### DELETING DATA

```c++
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
```

### SAVING DATA

```c++
	// save the data to a string
	std::string data;
	rc = ini.Save(data);
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_OK);

	// save the data back to the file
	rc = ini.SaveFile("example2.ini");
	if (rc < 0) { /* handle error */ };
	ASSERT_EQ(rc, SI_OK);
```
