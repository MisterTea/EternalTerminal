// File:    test1.cpp
// Library: SimpleIni
// Author:  Brodie Thiesfield <code@jellycan.com>
// Source:  http://code.jellycan.com/simpleini/
//
// Automated testing for SimpleIni streams

#ifdef _WIN32
# pragma warning(disable: 4786)
#endif

#ifdef _WIN32
# include <windows.h>
# define DELETE_FILE DeleteFileA
#else
# include <unistd.h>
# define DELETE_FILE unlink
#endif
#include <fstream>

#define SI_SUPPORT_IOSTREAMS
#include "SimpleIni.h"

class Test
{
    std::string m_strTest;

public:
    Test(const char * a_pszName)
        : m_strTest(a_pszName)
    {
        printf("%s: test starting\n", m_strTest.c_str());
    }

    bool Success()
    {
        printf("%s: test succeeded\n", m_strTest.c_str());
        return false;
    }

    bool Failure(const char * pszReason)
    {
        printf("%s: test FAILED (%s)\n", m_strTest.c_str(), pszReason);
        return false;
    }
};

bool FileComparisonTest(const char * a_pszFile1, const char * a_pszFile2) {
    // ensure that the two files are the same
    try {
        std::string strFile1, strFile2;

        char szBuf[1024];
        FILE * fp = NULL;

#if __STDC_WANT_SECURE_LIB__
		fopen_s(&fp, a_pszFile1, "rb");
#else
		fp = fopen(a_pszFile1, "rb");
#endif
        if (!fp) throw false;
        while (!feof(fp)) {
            size_t n = fread(szBuf, 1, sizeof(szBuf), fp);
            strFile1.append(szBuf, n);
        }
        fclose(fp);

		fp = NULL;
#if __STDC_WANT_SECURE_LIB__
		fopen_s(&fp, a_pszFile2, "rb");
#else
		fp = fopen(a_pszFile2, "rb");
#endif
        if (!fp) throw false;
        while (!feof(fp)) {
            size_t n = fread(szBuf, 1, sizeof(szBuf), fp);
            strFile2.append(szBuf, n);
        }
        fclose(fp);

        if (strFile1 != strFile2) throw false;
    }
    catch (...) {
        return false;
    }

    return true;
}

bool FileLoadTest(const char * a_pszFile1, const char * a_pszFile2) {
    // ensure that the two files load into simpleini the same
    CSimpleIniA ini(true, true, true);
    bool b;
    try {
        ini.Reset();
        if (ini.LoadFile(a_pszFile1) < 0) throw "Load failed for file 1";
        if (ini.SaveFile("test1.ini") < 0) throw "Save failed for file 1";

        ini.Reset();
        if (ini.LoadFile(a_pszFile2) < 0) throw "Load failed for file 2";
        if (ini.SaveFile("test2.ini") < 0) throw "Save failed for file 2";

        b = FileComparisonTest("test1.ini", "test2.ini");
        DELETE_FILE("test1.ini");
        DELETE_FILE("test2.ini");
        if (!b) throw "File comparison failed in FileLoadTest";
    }
    catch (...) {
        return false;
    }

    return true;
}

bool TestStreams()
{
    const char * rgszTestFile[3] = {
        "test1-input.ini",
        "test1-output.ini",
        "test1-expected.ini"
    };

    Test oTest("TestStreams");

    CSimpleIniW ini;
    ini.SetUnicode(true);
    ini.SetMultiKey(true);
    ini.SetMultiLine(true);

    // load the file
    try {
        std::ifstream instream;
        instream.open(rgszTestFile[0], std::ifstream::in | std::ifstream::binary);
        if (ini.LoadData(instream) < 0) throw false;
        instream.close();
    }
    catch (...) {
        return oTest.Failure("Failed to load file");
    }

    // standard contents test
    //if (!StandardContentsTest(ini, oTest)) {
    //    return false;
    //}

    // save the file
    try {
        std::ofstream outfile;
        outfile.open(rgszTestFile[1], std::ofstream::out | std::ofstream::binary);
        if (ini.Save(outfile, true) < 0) throw false;
        outfile.close();
    }
    catch (...) {
        return oTest.Failure("Failed to save file");
    }

    // file comparison test
    if (!FileComparisonTest(rgszTestFile[1], rgszTestFile[2])) {
        return oTest.Failure("Failed file comparison");
    }
    if (!FileLoadTest(rgszTestFile[1], rgszTestFile[2])) {
        return oTest.Failure("Failed file load comparison");
    }

    return oTest.Success();
}
