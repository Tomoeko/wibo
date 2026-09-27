#include <windows.h>

#include "test_assert.h"

typedef BOOL(WINAPI *match_fn)(LPCWSTR, LPCWSTR);

static const struct {
	const char *label;
	LPCWSTR file;
	LPCWSTR pattern;
	BOOL expected;
} cases[] = {
	{"empty-pattern", L"", L"", FALSE},
	{"empty-star", L"", L"*", TRUE},
	{"empty-all-files", L"", L"*.*", TRUE},
	{"empty-question", L"", L"?", FALSE},
	{"empty-space-pattern", L"", L" ", TRUE},
	{"empty-alternative", L"", L";", TRUE},
	{"literal-exact", L"Report.TXT", L"Report.TXT", TRUE},
	{"literal-case", L"Report.TXT", L"report.txt", TRUE},
	{"literal-mismatch", L"Report.TXT", L"report.doc", FALSE},
	{"extension", L"Report.TXT", L"*.txt", TRUE},
	{"no-extension-all-files", L"Report", L"*.*", TRUE},
	{"no-extension-leading-space", L"Report", L" *.*", FALSE},
	{"all-files-in-alternative", L"Report", L"*.doc;*.*", FALSE},
	{"suffix", L"file.txt", L"file.*", TRUE},
	{"empty-suffix", L"file", L"file.*", FALSE},
	{"question-one", L"ab", L"a?", TRUE},
	{"question-empty", L"a", L"a?", FALSE},
	{"star-middle", L"abbbc", L"a*c", TRUE},
	{"consecutive-star", L"abbbc", L"a**c", TRUE},
	{"backtrack", L"ababc", L"*ab*c", TRUE},
	{"empty-star-middle", L"ac", L"a*c", TRUE},
	{"leading-space", L"file.txt", L" *.txt", TRUE},
	{"trailing-space", L"file.txt", L"*.txt ", FALSE},
	{"second-alternative", L"file.txt", L"*.doc;*.txt", TRUE},
	{"spaced-alternative", L"file.txt", L"*.doc;  *.txt", TRUE},
	{"tab-alternative", L"file.txt", L"*.doc;\t*.txt", FALSE},
	{"leading-semicolon", L"file.txt", L";*.txt", TRUE},
	{"trailing-semicolon", L"file.txt", L"*.txt;", TRUE},
	{"separator-star", L"C:\\folder\\file.txt", L"*.txt", TRUE},
	{"separator-question", L"a\\b", L"a?b", TRUE},
	{"slash-literal", L"a/b", L"a\\b", FALSE},
	{"semicolon-name", L"a;b", L"a*b", TRUE},
	{"bracket-literal", L"[ab]", L"[ab]", TRUE},
	{"bracket-not-class", L"a", L"[ab]", FALSE},
	{"dot-literal", L"a", L"a.", FALSE},
	{"extension-empty", L"a.", L"*.", TRUE},
	{"leading-file-space", L" file.txt", L"file.txt", FALSE},
	{"latin-case", L"\x00e9.txt", L"\x00c9.TXT", TRUE},
	{"greek-case", L"\x03c9.txt", L"\x03a9.TXT", TRUE},
	{"final-sigma", L"\x03c2.txt", L"\x03a3.TXT", FALSE},
	{"dotless-i", L"\x0131.txt", L"I.TXT", FALSE},
	{"fullwidth-case", L"\xff41.txt", L"\xff21.TXT", TRUE},
	{"cjk-exact", L"\x65e5.txt", L"\x65e5.TXT", TRUE},
	{"surrogate-exact", L"\xd801\xdc28.txt", L"\xd801\xdc28.TXT", TRUE},
	{"surrogate-case", L"\xd801\xdc28.txt", L"\xd801\xdc00.TXT", FALSE},
	{"surrogate-question", L"\xd801\xdc28", L"?", FALSE},
	{"surrogate-two-questions", L"\xd801\xdc28", L"??", TRUE},
	{"lone-surrogate", L"\xd801.txt", L"\xd801.TXT", TRUE},
};

int main(int argc, char **argv) {
	char responseMode[32] = {0};
	DWORD responseLength =
		GetEnvironmentVariableA("WIBO_FIXTURE_PATH_MATCH_RESPONSE", responseMode, sizeof(responseMode));
	TEST_CHECK(responseLength < sizeof(responseMode));
	const BOOL asciiOnly =
		(argc > 1 && strcmp(argv[1], "ascii") == 0) || (responseLength && strcmp(responseMode, "success") != 0);
	HMODULE module = LoadLibraryA("shlwapi.dll");
	TEST_CHECK(module != NULL);
	FARPROC symbol = GetProcAddress(module, "PathMatchSpecW");
	match_fn match = NULL;
	TEST_CHECK(sizeof(match) == sizeof(symbol));
	memcpy(&match, &symbol, sizeof(match));
	TEST_CHECK(match != NULL);
	for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		if (asciiOnly && index >= 37)
			continue;
		WCHAR file[128], pattern[128];
		memset(file, 0xa5, sizeof(file));
		memset(pattern, 0xa5, sizeof(pattern));
		size_t fileUnits = 0, patternUnits = 0;
		while (cases[index].file[fileUnits])
			++fileUnits;
		while (cases[index].pattern[patternUnits])
			++patternUnits;
		TEST_CHECK(fileUnits + 1 < 128 && patternUnits + 1 < 128);
		memcpy(file, cases[index].file, (fileUnits + 1) * sizeof(WCHAR));
		memcpy(pattern, cases[index].pattern, (patternUnits + 1) * sizeof(WCHAR));
		WCHAR fileBefore[128], patternBefore[128];
		memcpy(fileBefore, file, sizeof(file));
		memcpy(patternBefore, pattern, sizeof(pattern));
		SetLastError(0x4321);
		BOOL result = match(file, pattern);
		DWORD error = GetLastError();
		printf("case=%u name=%s result=%d error=%lu\n", index, cases[index].label, result != FALSE,
			   (unsigned long)error);
		TEST_CHECK_EQ(cases[index].expected, result != FALSE);
		TEST_CHECK_EQ(0x4321, error);
		TEST_CHECK(memcmp(fileBefore, file, sizeof(file)) == 0);
		TEST_CHECK(memcmp(patternBefore, pattern, sizeof(pattern)) == 0);
	}
	if (responseLength && strcmp(responseMode, "success") != 0) {
		const DWORD expectedError = strcmp(responseMode, "failed") == 0		   ? ERROR_ACCESS_DENIED
									: strcmp(responseMode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED
																			   : ERROR_INVALID_DATA;
		SetLastError(0x4321);
		TEST_CHECK_EQ(FALSE, match(L"\x00e9.txt", L"\x00c9.TXT"));
		TEST_CHECK_EQ(expectedError, GetLastError());
	}
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
