#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef BOOL(WINAPI *IsCharAlphaWFunction)(WCHAR);

static const struct {
	WCHAR character;
	BOOL expected;
} unicodeCases[] = {
	{0x00aa, TRUE},	 {0x00ba, TRUE},  {0x00c0, TRUE},  {0x00df, TRUE},	{0x00e9, TRUE},	 {0x0100, TRUE},
	{0x0130, TRUE},	 {0x0178, TRUE},  {0x0301, FALSE}, {0x03a9, TRUE},	{0x03c0, TRUE},	 {0x0416, TRUE},
	{0x05d0, TRUE},	 {0x0627, TRUE},  {0x0905, TRUE},  {0x0e01, TRUE},	{0x200d, FALSE}, {0x2028, FALSE},
	{0x20ac, FALSE}, {0x3000, FALSE}, {0x3001, FALSE}, {0x3042, TRUE},	{0x30a2, TRUE},	 {0x4e2d, TRUE},
	{0xac00, TRUE},	 {0xd800, FALSE}, {0xd801, FALSE}, {0xdbff, FALSE}, {0xdc00, FALSE}, {0xdfff, FALSE},
	{0xe000, FALSE}, {0xff10, FALSE}, {0xff21, TRUE},  {0xff41, TRUE},	{0xfffd, FALSE}, {0xffff, FALSE}};

static void writeCharacterTable(IsCharAlphaWFunction classify, const char *path) {
	unsigned char table[0x10000 / 8] = {0};
	unsigned int letters = 0;
	for (unsigned int code = 0; code < 0x10000; ++code) {
		SetLastError(0x4321);
		const BOOL result = classify((WCHAR)code);
		TEST_CHECK_EQ(0x4321, GetLastError());
		if (result) {
			table[code / 8] |= (unsigned char)(1u << (code % 8));
			++letters;
		}
	}
	FILE *output = fopen(path, "wb");
	TEST_CHECK(output != NULL);
	TEST_CHECK_EQ(sizeof(table), fwrite(table, 1, sizeof(table), output));
	TEST_CHECK_EQ(0, fclose(output));
	printf("Full WCHAR domain: %u alphabetic code units\n", letters);
}

int main(int argc, char **argv) {
	HMODULE module = LoadLibraryA("user32.dll");
	TEST_CHECK(module != NULL);
	FARPROC procedure = GetProcAddress(module, "IsCharAlphaW");
	TEST_CHECK(procedure != NULL);
	IsCharAlphaWFunction classify;
	_Static_assert(sizeof(classify) == sizeof(procedure), "Function pointer width");
	memcpy(&classify, &procedure, sizeof(classify));

	unsigned int asciiLetters = 0;
	for (unsigned int code = 0; code < 128; ++code) {
		const BOOL expected = (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z');
		SetLastError(0x4321);
		const BOOL actual = classify((WCHAR)code);
		const DWORD error = GetLastError();
		TEST_CHECK_EQ(expected, actual != FALSE);
		TEST_CHECK_EQ(0x4321, error);
		asciiLetters += actual != FALSE;
	}
	TEST_CHECK_EQ(52, asciiLetters);

	char mode[32] = {0};
	const DWORD modeLength = GetEnvironmentVariableA("WIBO_FIXTURE_ALPHA_RESPONSE", mode, sizeof(mode));
	TEST_CHECK(modeLength < sizeof(mode));
	if (modeLength && strcmp(mode, "success") != 0) {
		const DWORD expectedError = strcmp(mode, "failed") == 0		   ? ERROR_ACCESS_DENIED
									: strcmp(mode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED
																	   : ERROR_INVALID_DATA;
		SetLastError(0x4321);
		TEST_CHECK_EQ(FALSE, classify(0x00e9));
		TEST_CHECK_EQ(expectedError, GetLastError());
		if (strcmp(mode, "unavailable") == 0) {
			TEST_CHECK(FreeLibrary(module));
			return 0;
		}
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_ALPHA_RESPONSE", "success"));
	}

	for (size_t index = 0; index < sizeof(unicodeCases) / sizeof(unicodeCases[0]); ++index) {
		SetLastError(0x4321);
		const BOOL actual = classify(unicodeCases[index].character);
		const DWORD error = GetLastError();
		TEST_CHECK_EQ(unicodeCases[index].expected, actual != FALSE);
		TEST_CHECK_EQ(0x4321, error);
	}
	if (modeLength) {
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_ALPHA_RESPONSE", "failed"));
		SetLastError(0x4321);
		TEST_CHECK_EQ(TRUE, classify(0xff21));
		TEST_CHECK_EQ(0x4321, GetLastError());
	}
	if (argc > 1) {
		TEST_CHECK_EQ(3, argc);
		TEST_CHECK_STR_EQ("--table", argv[1]);
		writeCharacterTable(classify, argv[2]);
	}
	TEST_CHECK(FreeLibrary(module));
	printf("IsCharAlphaW: %u ASCII and %u Unicode observations\n", 128u,
		   (unsigned int)(sizeof(unicodeCases) / sizeof(unicodeCases[0])));
	return 0;
}
