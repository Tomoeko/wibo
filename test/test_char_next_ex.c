#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef LPSTR(WINAPI *NextCharacterFunction)(WORD, LPCSTR, DWORD);

static BOOL isExpectedLeadByte(WORD codePage, unsigned int value) {
	if (codePage == 932)
		return (value >= 0x81 && value <= 0x9f) || (value >= 0xe0 && value <= 0xfc);
	if (codePage == 936 || codePage == 949 || codePage == 950)
		return value >= 0x81 && value <= 0xfe;
	if (codePage == 1361)
		return (value >= 0x84 && value <= 0xd3) || (value >= 0xd8 && value <= 0xde) || (value >= 0xe0 && value <= 0xf9);
	return FALSE;
}

int main(void) {
	HMODULE module = LoadLibraryA("user32.dll");
	TEST_CHECK(module != NULL);
	FARPROC procedure = GetProcAddress(module, "CharNextExA");
	TEST_CHECK(procedure != NULL);
	NextCharacterFunction next;
	_Static_assert(sizeof(next) == sizeof(procedure), "Function pointer width");
	memcpy(&next, &procedure, sizeof(next));
	const WORD codePages[] = {0, 932, 936, 949, 950, 1361, 1252, 28591, 65001};
	for (size_t page = 0; page < sizeof(codePages) / sizeof(codePages[0]); ++page) {
		unsigned int pairs = 0;
		for (unsigned int value = 0; value < 256; ++value) {
			char text[3] = {(char)value, 'A', 0};
			SetLastError(0x4321);
			LPSTR result = next(codePages[page], text, 0);
			const DWORD error = GetLastError();
			const unsigned int distance = !value ? 0 : isExpectedLeadByte(codePages[page], value) ? 2 : 1;
			TEST_CHECK(result == text + distance);
			TEST_CHECK_EQ(0x4321, error);
			pairs += distance == 2;
			SetLastError(0x4321);
			TEST_CHECK(next(codePages[page], text, 0xffffffff) == result);
			TEST_CHECK_EQ(0x4321, GetLastError());
			text[1] = 0;
			SetLastError(0x4321);
			result = next(codePages[page], text, 0);
			TEST_CHECK_EQ(value != 0, result - text);
			TEST_CHECK_EQ(0x4321, GetLastError());
		}
		printf("page=%u pairs=%u\n", codePages[page], pairs);
	}
	char empty[1] = {0};
	SetLastError(0x4321);
	TEST_CHECK(next(65535, empty, 0) == empty);
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
