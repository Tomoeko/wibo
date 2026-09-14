#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

// Read-only profile APIs. This fixture creates only exclusive private files in
// its working directory, then deletes those owned files. It never writes win.ini.
// Optional --system-profile <path> reads a separately provisioned owned win.ini:
// [Wibo.Profile.Read.Control]
// Integer=314159
// Text=" wrapper value "
// Negative=-12
//
// The default assertions follow documented Windows contracts. --wine-baseline
// explicitly asserts Wine 11's two measured discrepancies: the A enumeration
// conversion writes one trailing NUL on truncation, and GetProfileInt wraps a
// negative stored integer rather than returning the documented zero.
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprivateprofilestringw
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprofileinta
static const DWORD kSentinelError = 0x12345678;
static int wineBaseline;
static char privatePath[MAX_PATH], wideFilePath[MAX_PATH];
static WCHAR privatePathW[MAX_PATH], wideFilePathW[MAX_PATH];

static void widen_ascii(const char *input, WCHAR *output) {
	do {
		*output++ = (unsigned char)*input;
	} while (*input++);
}

static void cleanup(void) {
	if (privatePath[0])
		DeleteFileA(privatePath);
	if (wideFilePath[0])
		DeleteFileA(wideFilePath);
}

static void create_owned_file(char *path, const char *suffix, const void *bytes, DWORD size) {
	char candidate[MAX_PATH];
	HANDLE file = INVALID_HANDLE_VALUE;
	for (unsigned attempt = 0; attempt < 32; ++attempt) {
		sprintf(candidate, ".\\wibo-profile-%lu-%lu-%u-%s.ini", GetCurrentProcessId(), GetTickCount(), attempt, suffix);
		file = CreateFileA(candidate, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file != INVALID_HANDLE_VALUE)
			break;
		TEST_CHECK_EQ(ERROR_FILE_EXISTS, GetLastError());
	}
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	strcpy(path, candidate);
	DWORD written = 0;
	TEST_CHECK(WriteFile(file, bytes, size, &written, NULL));
	TEST_CHECK_EQ(size, written);
	TEST_CHECK(CloseHandle(file));
}

static void check_string(const char *key, const char *expected) {
	char result[128];
	WCHAR keyW[128], expectedW[128], resultW[128];
	widen_ascii(key, keyW);
	widen_ascii(expected, expectedW);
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(strlen(expected),
				  GetPrivateProfileStringA("example", key, "  fallback \t ", result, sizeof(result), privatePath));
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	TEST_CHECK_STR_EQ(expected, result);
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(wcslen(expectedW),
				  GetPrivateProfileStringW(L"EXAMPLE", keyW, L"  fallback \t ", resultW, 128, privatePathW));
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	TEST_CHECK_EQ(0, wcscmp(expectedW, resultW));
}

static void test_values(void) {
	check_string("Plain", "value");
	check_string("Double", " quoted ");
	check_string("Single", " quoted ");
	check_string("Empty", "");
	check_string("Spaces", "");
	check_string("Bare", "  fallback \t");
	check_string("Absent", "  fallback \t");
	check_string("Dup", "first");
	check_string("Semi", "value;remark");
	check_string("Later", "  fallback \t"); // The first matching section wins.
	const struct {
		const char *key;
		UINT expected;
	} cases[] = {{"Num", 102}, {"Neg", (UINT)-12}, {"Plus", 12},		{"Hex", 32},		  {"Oct", 10},
				 {"Bad", 0},   {"Overflow", 1},	   {"Empty", (UINT)-7}, {"Spaces", (UINT)-7}, {"Absent", (UINT)-7}};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		WCHAR keyW[128];
		widen_ascii(cases[i].key, keyW);
		SetLastError(kSentinelError);
		TEST_CHECK_EQ(cases[i].expected, GetPrivateProfileIntA("Example", cases[i].key, -7, privatePath));
		TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
		SetLastError(kSentinelError);
		TEST_CHECK_EQ(cases[i].expected, GetPrivateProfileIntW(L"Example", keyW, -7, privatePathW));
		TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	}
	TEST_CHECK_EQ(102, GetPrivateProfileIntA(" Example ", " Num ", 77, privatePath));
	TEST_CHECK_EQ(102, GetPrivateProfileIntW(L" Example ", L" Num ", 77, privatePathW));
	char result[32];
	TEST_CHECK_EQ(0, GetPrivateProfileStringA("Example", "Absent", NULL, result, sizeof(result), privatePath));
	TEST_CHECK_EQ(0, result[0]);
}

static void test_enumeration_and_buffers(void) {
	static const char sections[] = "Example\0Second\0Example\0";
	static const WCHAR sectionsW[] = L"Example\0Second\0Example\0";
	static const char keys[] =
		"Plain\0Double\0Single\0Empty\0Dup\0dup\0Semi\0Num\0Neg\0Plus\0Hex\0Oct\0Bad\0Overflow\0Spaces\0";
	char result[128];
	WCHAR resultW[128];
	memset(result, 0x55, sizeof(result));
	TEST_CHECK_EQ(sizeof(sections) - 1,
				  GetPrivateProfileStringA(NULL, NULL, "ignored", result, sizeof(result), privatePath));
	TEST_CHECK_EQ(0, memcmp(sections, result, sizeof(sections)));
	TEST_CHECK_EQ(0x55, result[sizeof(sections)]);
	TEST_CHECK_EQ(sizeof(sectionsW) / sizeof(WCHAR) - 1,
				  GetPrivateProfileStringW(NULL, NULL, L"ignored", resultW, 128, privatePathW));
	TEST_CHECK_EQ(0, memcmp(sectionsW, resultW, sizeof(sectionsW)));
	TEST_CHECK_EQ(sizeof(keys) - 1,
				  GetPrivateProfileStringA("Example", NULL, "ignored", result, sizeof(result), privatePath));
	TEST_CHECK_EQ(0, memcmp(keys, result, sizeof(keys)));
	TEST_CHECK_EQ(8, GetPrivateProfileStringA("Missing", NULL, "fallback", result, sizeof(result), privatePath));
	TEST_CHECK_STR_EQ("fallback", result);
	for (DWORD size = 0; size <= 6; ++size) {
		memset(result, 0x55, sizeof(result));
		SetLastError(kSentinelError);
		DWORD expected = size ? size - 1 : 0;
		TEST_CHECK_EQ(expected, GetPrivateProfileStringA("Example", "Plain", "ignored", result, size, privatePath));
		TEST_CHECK_EQ(size ? ERROR_SUCCESS : kSentinelError, GetLastError());
		if (size) {
			TEST_CHECK_EQ(0, memcmp("value", result, expected));
			TEST_CHECK_EQ(0, result[expected]);
		}
		TEST_CHECK_EQ(0x55, result[size]);

		memset(result, 0x55, sizeof(result));
		for (unsigned i = 0; i < 128; ++i)
			resultW[i] = 0x5555;
		expected = size > 1 ? size - 2 : 0;
		SetLastError(kSentinelError);
		TEST_CHECK_EQ(expected, GetPrivateProfileStringA(NULL, NULL, "ignored", result, size, privatePath));
		TEST_CHECK_EQ(size ? ERROR_SUCCESS : kSentinelError, GetLastError());
		SetLastError(kSentinelError);
		TEST_CHECK_EQ(expected, GetPrivateProfileStringW(NULL, NULL, L"ignored", resultW, size, privatePathW));
		TEST_CHECK_EQ(size ? ERROR_SUCCESS : kSentinelError, GetLastError());
		if (size) {
			TEST_CHECK_EQ(0, memcmp(sections, result, expected));
			TEST_CHECK_EQ(0, memcmp(sectionsW, resultW, expected * sizeof(WCHAR)));
			TEST_CHECK_EQ(0, result[expected]);
			TEST_CHECK_EQ(0, resultW[expected]);
			if (size > 1) {
				TEST_CHECK_EQ(wineBaseline ? 0x55 : 0, result[expected + 1]);
				TEST_CHECK_EQ(0, resultW[expected + 1]);
			}
		}
		TEST_CHECK_EQ(0x55, result[size]);
		TEST_CHECK_EQ(0x5555, resultW[size]);
	}
}

static void test_missing_file_and_unicode(void) {
	char missing[MAX_PATH + 9], result[128];
	WCHAR missingW[MAX_PATH + 9], resultW[128];
	sprintf(missing, "%s.missing", privatePath);
	widen_ascii(missing, missingW);
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(91, GetPrivateProfileIntA("Example", "Num", 91, missing));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(91, GetPrivateProfileIntW(L"Example", L"Num", 91, missingW));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(8, GetPrivateProfileStringA("Example", "Num", "fallback", result, sizeof(result), missing));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	TEST_CHECK_STR_EQ("fallback", result);
	memset(result, 0x55, sizeof(result));
	TEST_CHECK_EQ(0, GetPrivateProfileStringA(NULL, NULL, "ignored", result, sizeof(result), missing));
	TEST_CHECK_EQ(0, result[0]);
	TEST_CHECK_EQ(0x55, result[1]);
	TEST_CHECK_EQ(2, GetPrivateProfileStringW(L"Wide", L"Value", L"fallback", resultW, 128, wideFilePathW));
	TEST_CHECK_EQ(0x96ea, resultW[0]);
	TEST_CHECK_EQ(0xe9, resultW[1]);
	TEST_CHECK_EQ(0, resultW[2]);
	TEST_CHECK_EQ(2, GetPrivateProfileStringA("Wide", "Value", "fallback", result, sizeof(result), wideFilePath));
	TEST_CHECK_EQ('?', result[0]);
	TEST_CHECK_EQ(0xe9, (unsigned char)result[1]);
	TEST_CHECK_EQ(0, result[2]);
	// A bare filename resolves in the Windows directory; .\\ resolves here.
	TEST_CHECK_EQ(77, GetPrivateProfileIntA("Example", "Num", 77, privatePath + 2));
}

static void test_profile_defaults(void) {
	char section[128], result[64];
	WCHAR sectionW[128], resultW[64];
	sprintf(section, "Wibo.Profile.Absent.%lu.%lu", GetCurrentProcessId(), GetTickCount());
	widen_ascii(section, sectionW);
	TEST_CHECK_EQ(192, GetProfileIntA(section, "absent", 192));
	TEST_CHECK_EQ(193, GetProfileIntW(sectionW, L"absent", 193));
	TEST_CHECK_EQ(8, GetProfileStringA(section, "absent", "fallback", result, sizeof(result)));
	TEST_CHECK_STR_EQ("fallback", result);
	TEST_CHECK_EQ(8, GetProfileStringW(sectionW, L"absent", L"fallback", resultW, 64));
	TEST_CHECK_EQ(0, wcscmp(L"fallback", resultW));
}

static void test_supplied_system_profile(const char *path) {
	WCHAR pathW[MAX_PATH], resultW[64];
	char result[64];
	TEST_CHECK(strlen(path) < MAX_PATH);
	widen_ascii(path, pathW);
	TEST_CHECK_EQ(314159, GetPrivateProfileIntA("Wibo.Profile.Read.Control", "Integer", -1, path));
	TEST_CHECK_EQ(314159, GetPrivateProfileIntW(L"Wibo.Profile.Read.Control", L"Integer", -1, pathW));
	TEST_CHECK_EQ(314159, GetProfileIntA("Wibo.Profile.Read.Control", "Integer", -1));
	TEST_CHECK_EQ(314159, GetProfileIntW(L"Wibo.Profile.Read.Control", L"Integer", -1));
	TEST_CHECK_EQ(15, GetProfileStringA("Wibo.Profile.Read.Control", "Text", "missing", result, sizeof(result)));
	TEST_CHECK_STR_EQ(" wrapper value ", result);
	TEST_CHECK_EQ(15, GetProfileStringW(L"Wibo.Profile.Read.Control", L"Text", L"missing", resultW, 64));
	TEST_CHECK_EQ(0, wcscmp(L" wrapper value ", resultW));
	TEST_CHECK_EQ(wineBaseline ? (UINT)-12 : 0, GetProfileIntA("Wibo.Profile.Read.Control", "Negative", -1));
	TEST_CHECK_EQ(wineBaseline ? (UINT)-12 : 0, GetProfileIntW(L"Wibo.Profile.Read.Control", L"Negative", -1));
	printf("supplied system profile passed (%s contracts)\n", wineBaseline ? "Wine baseline" : "documented");
}

int main(int argc, char **argv) {
	const char *systemProfile = NULL;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--wine-baseline"))
			wineBaseline = 1;
		else if (!strcmp(argv[i], "--system-profile") && i + 1 < argc)
			systemProfile = argv[++i];
		else
			TEST_FAIL("Unknown or incomplete argument: %s", argv[i]);
	}
	atexit(cleanup);
	static const char ini[] =
		"; comment\r\n[Example]\r\nPlain=  value  \r\nDouble=\" quoted \"\r\nSingle=' quoted '\r\n"
		"Empty=\r\nBare\r\nDup=first\r\ndup=second\r\nSemi=value;remark\r\nNum=102abc\r\nNeg=-12\r\n"
		"Plus=+12\r\nHex=0x20\r\nOct=010\r\nBad=word\r\nOverflow=4294967297\r\nSpaces=   \r\n"
		"[Second]\r\nK=v\r\n[Example]\r\nLater=later\r\n";
	static const unsigned char wideIni[] = {0xff, 0xfe, '[',  0, 'W',  0,	 'i',  0, 'd',	0, 'e', 0,
											']',  0,	'\n', 0, 'V',  0,	 'a',  0, 'l',	0, 'u', 0,
											'e',  0,	'=',  0, 0xea, 0x96, 0xe9, 0, '\n', 0};
	create_owned_file(privatePath, "ansi", ini, sizeof(ini) - 1);
	create_owned_file(wideFilePath, "wide", wideIni, sizeof(wideIni));
	widen_ascii(privatePath, privatePathW);
	widen_ascii(wideFilePath, wideFilePathW);
	test_values();
	test_enumeration_and_buffers();
	test_missing_file_and_unicode();
	test_profile_defaults();
	if (systemProfile)
		test_supplied_system_profile(systemProfile);
	printf("profile reads passed (%s contracts)\n", wineBaseline ? "Wine baseline" : "documented");
	return 0;
}
