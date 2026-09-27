#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef UINT(WINAPI *OemCodePageFunction)(void);
typedef BOOL(WINAPI *StringTypeFunction)(LCID, DWORD, LPCSTR, int, LPWORD);
typedef BOOL(WINAPI *LocaleNameFunction)(LPCWSTR);

static void loadProcedure(HMODULE module, const char *name, void *destination, size_t size) {
	FARPROC procedure = GetProcAddress(module, name);
	TEST_CHECK(procedure != NULL);
	TEST_CHECK_EQ(sizeof(procedure), size);
	memcpy(destination, &procedure, size);
}

static void checkTransportFailure(OemCodePageFunction getOemCodePage, StringTypeFunction classify,
								  LocaleNameFunction validate, const char *mode) {
	const DWORD expectedError = strcmp(mode, "failed") == 0		   ? ERROR_ACCESS_DENIED
								: strcmp(mode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED
																   : ERROR_INVALID_DATA;
	SetLastError(0x4321);
	TEST_CHECK_EQ(0, getOemCodePage());
	TEST_CHECK_EQ(expectedError, GetLastError());
	const char source[] = {'A', 'a', 0};
	WORD output[] = {0xa5a5, 0xa5a5, 0xa5a5, 0xa5a5, 0xa5a5};
	SetLastError(0x4321);
	TEST_CHECK_EQ(FALSE, classify(0x0409, CT_CTYPE1, source, 3, output + 1));
	TEST_CHECK_EQ(expectedError, GetLastError());
	for (size_t index = 0; index < sizeof(output) / sizeof(output[0]); ++index)
		TEST_CHECK_EQ(0xa5a5, output[index]);
	SetLastError(0x4321);
	TEST_CHECK_EQ(FALSE, validate(L"en-US"));
	TEST_CHECK_EQ(expectedError, GetLastError());
}

static void checkOemCodePage(OemCodePageFunction getOemCodePage, BOOL mock) {
	CPINFOEXW information;
	memset(&information, 0, sizeof(information));
	TEST_CHECK(GetCPInfoExW(CP_OEMCP, 0, &information));
	TEST_CHECK(information.CodePage > CP_THREAD_ACP);
	for (unsigned int index = 0; index < 2; ++index) {
		SetLastError(0x4321 + index);
		const UINT codePage = getOemCodePage();
		TEST_CHECK_EQ(information.CodePage, codePage);
		TEST_CHECK_EQ(0x4321 + index, GetLastError());
	}
	if (mock) {
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NLS_SCALAR_RESPONSE", "failed"));
		SetLastError(0x4321);
		TEST_CHECK_EQ(information.CodePage, getOemCodePage());
		TEST_CHECK_EQ(0x4321, GetLastError());
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NLS_SCALAR_RESPONSE", "success"));
	}
}

static WORD asciiClassification(unsigned int code) {
	WORD type = C1_DEFINED;
	if (code < 32 || code == 127)
		type |= C1_CNTRL;
	if (code == ' ' || (code >= '\t' && code <= '\r'))
		type |= C1_SPACE;
	if (code == ' ' || code == '\t')
		type |= C1_BLANK;
	if (code >= '0' && code <= '9')
		type |= C1_DIGIT | C1_XDIGIT;
	else if (code >= 'A' && code <= 'Z')
		type |= C1_UPPER | C1_ALPHA | (code <= 'F' ? C1_XDIGIT : 0);
	else if (code >= 'a' && code <= 'z')
		type |= C1_LOWER | C1_ALPHA | (code <= 'f' ? C1_XDIGIT : 0);
	else if (code > ' ' && code < 127)
		type |= C1_PUNCT;
	return type;
}

static void checkAsciiDomain(StringTypeFunction classify) {
	char source[128];
	WORD output[130];
	for (unsigned int code = 0; code < 128; ++code)
		source[code] = (char)code;
	for (size_t index = 0; index < sizeof(output) / sizeof(output[0]); ++index)
		output[index] = 0xa5a5;
	SetLastError(0x4321);
	TEST_CHECK(classify(0x0409, CT_CTYPE1, source, 128, output + 1));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK_EQ(0xa5a5, output[0]);
	TEST_CHECK_EQ(0xa5a5, output[129]);
	for (unsigned int code = 0; code < 128; ++code)
		TEST_CHECK_EQ(asciiClassification(code), output[code + 1]);
}

static void checkClassification(StringTypeFunction classify, const char *name, LCID locale, const char *source,
								int count, const WORD *expected, size_t words, size_t stride) {
	const DWORD types[] = {CT_CTYPE1, CT_CTYPE2, CT_CTYPE3};
	TEST_CHECK(words <= stride && stride <= 8);
	for (size_t typeIndex = 0; typeIndex < sizeof(types) / sizeof(types[0]); ++typeIndex) {
		WORD output[12];
		for (size_t index = 0; index < sizeof(output) / sizeof(output[0]); ++index)
			output[index] = 0xa5a5;
		SetLastError(0x4321);
		TEST_CHECK(classify(locale, types[typeIndex], source, count, output + 1));
		TEST_CHECK_EQ(0x4321, GetLastError());
		TEST_CHECK_EQ(0xa5a5, output[0]);
		for (size_t index = 0; index < words; ++index)
			TEST_CHECK_EQ(expected[typeIndex * stride + index], output[index + 1]);
		for (size_t index = words + 1; index < sizeof(output) / sizeof(output[0]); ++index)
			TEST_CHECK_EQ(0xa5a5, output[index]);
		printf("%s type=%lu words=%u verified\n", name, (unsigned long)types[typeIndex], (unsigned int)words);
	}
}

static void checkClassificationEdges(StringTypeFunction classify) {
	const char source[] = {'a', 0};
	WORD output[4] = {0xa5a5, 0xa5a5, 0xa5a5, 0xa5a5};
	SetLastError(0x4321);
	TEST_CHECK(classify(0x0409, CT_CTYPE1, source, 0, output + 1));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(0x4321);
	TEST_CHECK_EQ(FALSE, classify(0x0409, 0, source, 1, output + 1));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	for (size_t index = 0; index < sizeof(output) / sizeof(output[0]); ++index)
		TEST_CHECK_EQ(0xa5a5, output[index]);
}

static void checkStringTypes(StringTypeFunction classify) {
	static const char ascii[] = {'A', 'a', '0', ' ', '\t', '@', 0, 'b', 0};
	static const WORD asciiExpected[] = {0x0381, 0x0382, 0x0284, 0x0248, 0x0268, 0x0210, 0x0220, 0x0382,
										 0x0001, 0x0001, 0x0003, 0x000a, 0x0009, 0x000b, 0x0000, 0x0001,
										 0x8040, 0x8040, 0x0040, 0x0048, 0x0008, 0x0448, 0x0000, 0x8040};
	static const char latin[] = {(char)0x80, (char)0xe9, (char)0xdf, (char)0xff, 0, 'A'};
	static const WORD latinExpected[] = {0x0200, 0x0302, 0x0302, 0x0302, 0x0220, 0x0381, 0x0005, 0x0001, 0x0001,
										 0x0001, 0x0000, 0x0001, 0x0008, 0x8003, 0x8000, 0x8003, 0x0000, 0x8040};
	static const char japanese[] = {(char)0x82, (char)0xa0, 'A', 0, 'B', 0};
	static const WORD japaneseExpected[] = {0x0300, 0x0381, 0x0220, 0x0381, 0x0220, 0x0001, 0x0001, 0x0000,
											0x0001, 0x0000, 0x8020, 0x8040, 0x0000, 0x8040, 0x0000};
	checkAsciiDomain(classify);
	checkClassification(classify, "ASCII-counted", 0x0409, ascii, 8, asciiExpected, 8, 8);
	checkClassification(classify, "ASCII-terminated", 0x0409, ascii, -1, asciiExpected, 7, 8);
	checkClassification(classify, "Latin-counted", 0x0409, latin, 6, latinExpected, 6, 6);
	checkClassification(classify, "DBCS-counted", 0x0411, japanese, 6, japaneseExpected, 5, 5);
	checkClassification(classify, "DBCS-terminated", 0x0411, japanese, -1, japaneseExpected, 3, 5);
	checkClassificationEdges(classify);
}

static void checkLocaleName(LocaleNameFunction validate, LPCWSTR locale, BOOL expected) {
	SetLastError(0x4321);
	TEST_CHECK_EQ(expected, validate(locale) != FALSE);
	TEST_CHECK_EQ(0x4321, GetLastError());
}

int main(void) {
	HMODULE module = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(module != NULL);
	OemCodePageFunction getOemCodePage;
	StringTypeFunction classify;
	LocaleNameFunction validate;
	loadProcedure(module, "GetOEMCP", &getOemCodePage, sizeof(getOemCodePage));
	loadProcedure(module, "GetStringTypeExA", &classify, sizeof(classify));
	loadProcedure(module, "IsValidLocaleName", &validate, sizeof(validate));
	char mode[32] = {0};
	const DWORD modeLength = GetEnvironmentVariableA("WIBO_FIXTURE_NLS_SCALAR_RESPONSE", mode, sizeof(mode));
	TEST_CHECK(modeLength < sizeof(mode));
	if (modeLength && strcmp(mode, "success") != 0) {
		checkTransportFailure(getOemCodePage, classify, validate, mode);
		if (strcmp(mode, "unavailable") == 0)
			return 0;
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NLS_SCALAR_RESPONSE", "success"));
	}
	checkOemCodePage(getOemCodePage, modeLength != 0);
	if (modeLength && strcmp(mode, "success") != 0) {
		static const char retrySource[] = {(char)0x82, (char)0xa0, 'A', 0};
		static const WORD retryExpected[] = {0x0300, 0x0381, 0x0220, 0x0001, 0x0001, 0x0000, 0x8020, 0x8040, 0x0000};
		checkClassification(classify, "retry", 0x0411, retrySource, -1, retryExpected, 3, 3);
		checkLocaleName(validate, L"en-US", TRUE);
		return 0;
	}
	checkStringTypes(classify);
	checkLocaleName(validate, L"", TRUE);
	checkLocaleName(validate, L"en", TRUE);
	checkLocaleName(validate, L"en-US", TRUE);
	checkLocaleName(validate, L"EN-us", TRUE);
	checkLocaleName(validate, L"ja-JP", TRUE);
	checkLocaleName(validate, L"de-DE_phoneb", TRUE);
	checkLocaleName(validate, L"invalid-locale-zz", FALSE);
	checkLocaleName(validate, L"\x65e5", FALSE);
	checkLocaleName(validate, NULL, FALSE);
	WCHAR longName[90];
	for (size_t index = 0; index + 1 < sizeof(longName) / sizeof(longName[0]); ++index)
		longName[index] = 'a';
	longName[89] = 0;
	checkLocaleName(validate, longName, FALSE);
	puts("NLS scalar fixture complete");
	return 0;
}
