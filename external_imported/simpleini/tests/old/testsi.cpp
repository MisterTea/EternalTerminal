// File:    testsi.cpp
// Library: SimpleIni
// Author:  Brodie Thiesfield <code@jellycan.com>
// Source:  http://code.jellycan.com/simpleini/
//
// Demo of usage

#ifdef _WIN32
# pragma warning(disable: 4786)
#endif

#include <locale.h>
#include <stdio.h>
#include <cassert>

#define SI_SUPPORT_IOSTREAMS
#if defined(SI_SUPPORT_IOSTREAMS) && !defined(_UNICODE)
# include <fstream>
#endif

//#define SI_CONVERT_GENERIC
//#define SI_CONVERT_ICU
//#define SI_CONVERT_WIN32
#include "../SimpleIni.h"

#ifdef SI_CONVERT_ICU
// if converting using ICU then we need the ICU library
# pragma comment(lib, "icuuc.lib")
#endif

#ifdef _WIN32
# include <tchar.h>
#else // !_WIN32
# define TCHAR      char
# define _T(x)      x
# define _tprintf   printf
# define _tmain     main
#endif // _WIN32

static void
Test(
    CSimpleIni &    ini
    )
{
    const TCHAR *pszSection = 0;
    const TCHAR *pItem = 0;
    const TCHAR *pszVal = 0;

    // get the value of the key "foo" in section "standard"
    bool bHasMulti;
    pszVal = ini.GetValue(_T("standard"), _T("foo"), 0, &bHasMulti);
    _tprintf(_T("\n-- Value of standard::foo is '%s' (hasMulti = %d)\n"),
        pszVal ? pszVal : _T("(null)"), bHasMulti);

    // set the value of the key "foo" in section "standard"
    ini.SetValue(_T("standard"), _T("foo"), _T("wibble"));
    pszVal = ini.GetValue(_T("standard"), _T("foo"), 0, &bHasMulti);
    _tprintf(_T("\n-- Value of standard::foo is '%s' (hasMulti = %d)\n"),
        pszVal ? pszVal : _T("(null)"), bHasMulti);

    // get all values of the key "foo" in section "standard"
    CSimpleIni::TNamesDepend values;
    if (ini.GetAllValues(_T("standard"), _T("foo"), values)) {
        _tprintf(_T("\n-- Values of standard::foo are:\n"));
        CSimpleIni::TNamesDepend::const_iterator i = values.begin();
        for (; i != values.end(); ++i) {
            pszVal = i->pItem;
            _tprintf(_T("   -> '%s'\n"), pszVal);
        }
    }

    // get the size of the section [standard]
    _tprintf(_T("\n-- Number of keys in section [standard] = %d\n"),
        ini.GetSectionSize(_T("standard")));

    // delete the key "foo" in section "standard", if it has value "bar"
    ini.DeleteValue(_T("standard"), _T("foo"), _T("bar"));
    pszVal = ini.GetValue(_T("standard"), _T("foo"), 0);
    _tprintf(_T("\n-- Value of standard::foo is now '%s'\n"),
        pszVal ? pszVal : _T("(null)"));

    // delete the key "foo" in section "standard"
    ini.Delete(_T("standard"), _T("foo"));
    pszVal = ini.GetValue(_T("standard"), _T("foo"), 0);
    _tprintf(_T("\n-- Value of standard::foo is now '%s'\n"),
        pszVal ? pszVal : _T("(null)"));

    // get the size of the section [standard]
    _tprintf(_T("\n-- Number of keys in section [standard] = %d\n"),
        ini.GetSectionSize(_T("standard")));

    // get the list of all key names for the section "standard"
    _tprintf(_T("\n-- Dumping keys of section: [standard]\n"));
    CSimpleIni::TNamesDepend keys;
    ini.GetAllKeys(_T("standard"), keys);

    // dump all of the key names
    CSimpleIni::TNamesDepend::const_iterator iKey = keys.begin();
    for ( ; iKey != keys.end(); ++iKey ) {
        pItem = iKey->pItem;
        _tprintf(_T("Key: %s\n"), pItem);
    }

    // add a decimal value
    ini.SetLongValue(_T("integer"), _T("dec"), 42, NULL, false);
    ini.SetLongValue(_T("integer"), _T("hex"), 42, NULL, true);

    // add some bool values
    ini.SetBoolValue(_T("bool"), _T("t"), true);
    ini.SetBoolValue(_T("bool"), _T("f"), false);

    // get the values back
    assert(42 == ini.GetLongValue(_T("integer"), _T("dec")));
    assert(42 == ini.GetLongValue(_T("integer"), _T("hex")));
    assert(true  == ini.GetBoolValue(_T("bool"), _T("t")));
    assert(false == ini.GetBoolValue(_T("bool"), _T("f")));

    // delete the section "standard"
    ini.Delete(_T("standard"), NULL);
    _tprintf(_T("\n-- Number of keys in section [standard] = %d\n"),
        ini.GetSectionSize(_T("standard")));

    // iterate through every section in the file
    _tprintf(_T("\n-- Dumping all sections\n"));
    CSimpleIni::TNamesDepend sections;
    ini.GetAllSections(sections);
    CSimpleIni::TNamesDepend::const_iterator iSection = sections.begin();
    for ( ; iSection != sections.end(); ++iSection ) {
        pszSection = iSection->pItem;

        // print the section name
        printf("\n");
        if (*pszSection) {
            _tprintf(_T("[%s]\n"), pszSection);
        }

        // if there are keys and values...
        const CSimpleIni::TKeyVal * pSectionData = ini.GetSection(pszSection);
        if (pSectionData) {
            // iterate over all keys and dump the key name and value
            CSimpleIni::TKeyVal::const_iterator iKeyVal = pSectionData->begin();
            for ( ;iKeyVal != pSectionData->end(); ++iKeyVal) {
                pItem = iKeyVal->first.pItem;
                pszVal = iKeyVal->second;
                _tprintf(_T("%s=%s\n"), pItem, pszVal);
            }
        }
    }
}

#if defined(SI_SUPPORT_IOSTREAMS) && !defined(_UNICODE)
static bool
TestStreams(
    const TCHAR *   a_pszFile,
    bool            a_bIsUtf8,
    bool            a_bUseMultiKey,
    bool            a_bUseMultiLine
    )
{
    // load the file
    CSimpleIni ini(a_bIsUtf8, a_bUseMultiKey, a_bUseMultiLine);
    _tprintf(_T("Loading file: %s\n"), a_pszFile);
    std::ifstream instream;
    instream.open(a_pszFile, std::ifstream::in | std::ifstream::binary);
    SI_Error rc = ini.LoadData(instream);
    instream.close();
    if (rc < 0) {
        printf("Failed to open file.\n");
        return false;
    }

    Test(ini);

    // save the file (simple)
    _tprintf(_T("\n-- Saving file to: testsi-out-streams.ini\n"));
    std::ofstream outstream;
    outstream.open("testsi-out-streams.ini", std::ofstream::out | std::ofstream::binary);
    ini.Save(outstream);
    outstream.close();

    return true;
}
#endif // SI_SUPPORT_IOSTREAMS

static bool
TestFile(
    const TCHAR *   a_pszFile,
    bool            a_bIsUtf8,
    bool            a_bUseMultiKey,
    bool            a_bUseMultiLine
    )
{
    // load the file
    CSimpleIni ini(a_bIsUtf8, a_bUseMultiKey, a_bUseMultiLine);
    _tprintf(_T("Loading file: %s\n"), a_pszFile);
    SI_Error rc = ini.LoadFile(a_pszFile);
    if (rc < 0) {
        printf("Failed to open file.\n");
        return false;
    }

    // run the tests
    Test(ini);

    // save the file (simple)
    _tprintf(_T("\n-- Saving file to: testsi-out.ini\n"));
    ini.SaveFile("testsi-out.ini");

    // save the file (with comments)
    // Note: to save the file and add a comment to the beginning, use
    // code such as the following.
    _tprintf(_T("\n-- Saving file to: testsi-out-comment.ini\n"));
	FILE * fp = NULL;
#if __STDC_WANT_SECURE_LIB__
	fopen_s(&fp, "testsi-out-comment.ini", "wb");
#else
	fp = fopen("testsi-out-comment.ini", "wb");
#endif
    if (fp) {
        CSimpleIni::FileWriter writer(fp);
        if (a_bIsUtf8) {
            writer.Write(SI_UTF8_SIGNATURE);
        }

        // add a string to the file in the correct text format
        CSimpleIni::Converter convert = ini.GetConverter();
        convert.ConvertToStore(_T("; output from testsi.cpp test program")
            SI_NEWLINE SI_NEWLINE);
        writer.Write(convert.Data());

        ini.Save(writer, false);
        fclose(fp);
    }

    return true;
}

static bool
ParseCommandLine(
    int                 argc,
    TCHAR *             argv[],
    const TCHAR * &     a_pszFile,
    bool &              a_bIsUtf8,
    bool &              a_bUseMultiKey,
    bool &              a_bUseMultiLine
    )
{
    a_pszFile = 0;
    a_bIsUtf8 = false;
    a_bUseMultiKey = false;
    a_bUseMultiLine = false;
    for (--argc; argc > 0; --argc) {
        if (argv[argc][0] == '-') {
            switch (argv[argc][1]) {
            case TCHAR('u'):
                a_bIsUtf8 = true;
                break;
            case TCHAR('m'):
                a_bUseMultiKey = true;
                break;
            case TCHAR('l'):
                a_bUseMultiLine = true;
                break;
            }
        }
        else {
            a_pszFile = argv[argc];
        }
    }
    if (!a_pszFile) {
        _tprintf(
            _T("Usage: testsi [-u] [-m] [-l] iniFile\n")
            _T("   -u  Load file as UTF-8 (Default is to use system locale)\n")
            _T("   -m  Enable multiple keys\n")
            _T("   -l  Enable multiple line values\n")
            );
        return false;
    }

    return true;
}

extern bool TestStreams();

int
_tmain(
    int     argc,
    TCHAR * argv[]
    )
{
    setlocale(LC_ALL, "");

    // start of automated testing...
    TestStreams();

    // parse the command line
    const TCHAR * pszFile;
    bool bIsUtf8, bUseMultiKey, bUseMultiLine;
    if (!ParseCommandLine(argc, argv, pszFile, bIsUtf8, bUseMultiKey, bUseMultiLine)) {
        return 1;
    }

    // run the test
    if (!TestFile(pszFile, bIsUtf8, bUseMultiKey, bUseMultiLine)) {
        return 1;
    }
#if defined(SI_SUPPORT_IOSTREAMS) && !defined(_UNICODE)
    if (!TestStreams(pszFile, bIsUtf8, bUseMultiKey, bUseMultiLine)) {
        return 1;
    }
#endif

    return 0;
}

