#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef DWORD(WINAPI *UpperBufferFunction)(LPWSTR, DWORD);

static void checkBuffer(UpperBufferFunction upper, const char *name, const WCHAR *input, const WCHAR *expected,
						size_t units, DWORD count) {
	WCHAR buffer[32];
	TEST_CHECK(units + 2 <= sizeof(buffer) / sizeof(buffer[0]));
	TEST_CHECK(count <= units);
	for (size_t index = 0; index < sizeof(buffer) / sizeof(buffer[0]); ++index)
		buffer[index] = 0x5a5a;
	memcpy(buffer + 1, input, units * sizeof(WCHAR));
	SetLastError(0x4321);
	const DWORD result = upper(buffer + 1, count);
	const DWORD error = GetLastError();
	TEST_CHECK_EQ(count, result);
	TEST_CHECK_EQ(count ? 0x4321 : ERROR_INVALID_PARAMETER, error);
	TEST_CHECK_EQ(0x5a5a, buffer[0]);
	for (size_t index = 0; index < count; ++index)
		TEST_CHECK_EQ(expected[index], buffer[index + 1]);
	for (size_t index = count; index < units; ++index)
		TEST_CHECK_EQ(input[index], buffer[index + 1]);
	for (size_t index = units + 1; index < sizeof(buffer) / sizeof(buffer[0]); ++index)
		TEST_CHECK_EQ(0x5a5a, buffer[index]);
	printf("%s count=%lu result=%lu error=%lu output=", name, (unsigned long)count, (unsigned long)result,
		   (unsigned long)error);
	for (size_t index = 0; index < units; ++index)
		printf("%s%04x", index ? "," : "", buffer[index + 1]);
	putchar('\n');
}

static void checkTransportFailure(UpperBufferFunction upper, const char *mode) {
	WCHAR buffer[] = {0x5a5a, 'a', 0, 'b', 0x00e9, 0x5a5a};
	const WCHAR original[] = {0x5a5a, 'a', 0, 'b', 0x00e9, 0x5a5a};
	const DWORD expectedError = strcmp(mode, "failed") == 0		   ? ERROR_ACCESS_DENIED
								: strcmp(mode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED
																   : ERROR_INVALID_DATA;
	SetLastError(0x4321);
	TEST_CHECK_EQ(0, upper(buffer + 1, 4));
	TEST_CHECK_EQ(expectedError, GetLastError());
	TEST_CHECK(memcmp(buffer, original, sizeof(buffer)) == 0);
}

static void checkAsciiDomain(UpperBufferFunction upper) {
	WCHAR buffer[130];
	buffer[0] = buffer[129] = 0x5a5a;
	for (unsigned int code = 0; code < 128; ++code)
		buffer[code + 1] = (WCHAR)code;
	SetLastError(0x4321);
	TEST_CHECK_EQ(128, upper(buffer + 1, 128));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK_EQ(0x5a5a, buffer[0]);
	TEST_CHECK_EQ(0x5a5a, buffer[129]);
	for (unsigned int code = 0; code < 128; ++code) {
		const WCHAR expected = code >= 'a' && code <= 'z' ? (WCHAR)(code - 0x20) : (WCHAR)code;
		TEST_CHECK_EQ(expected, buffer[code + 1]);
	}
	puts("ASCII domain: 128 counted code units verified");
}

int main(void) {
	HMODULE module = LoadLibraryA("user32.dll");
	TEST_CHECK(module != NULL);
	FARPROC procedure = GetProcAddress(module, "CharUpperBuffW");
	TEST_CHECK(procedure != NULL);
	UpperBufferFunction upper;
	_Static_assert(sizeof(upper) == sizeof(procedure), "Function pointer width");
	memcpy(&upper, &procedure, sizeof(upper));
	char mode[32] = {0};
	const DWORD modeLength = GetEnvironmentVariableA("WIBO_FIXTURE_UPPER_RESPONSE", mode, sizeof(mode));
	TEST_CHECK(modeLength < sizeof(mode));
	if (modeLength && strcmp(mode, "success") != 0) {
		checkTransportFailure(upper, mode);
		if (strcmp(mode, "unavailable") == 0) {
			TEST_CHECK(FreeLibrary(module));
			return 0;
		}
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_UPPER_RESPONSE", "success"));
	}
	static const WCHAR ascii[] = {'a', 'z', 'A', 'Z', '0', 0, 'b', 'i'};
	static const WCHAR asciiUpper[] = {'A', 'Z', 'A', 'Z', '0', 0, 'B', 'I'};
	static const WCHAR latin[] = {0x00e9, 0x00df, 0x00ff, 0x0131, 0x0130, 0x0149, 0x017f, 0xfb00};
	static const WCHAR latinUpper[] = {0x00c9, 0x00df, 0x0178, 0x0131, 0x0130, 0x0149, 0x017f, 0xfb00};
	static const WCHAR scripts[] = {0x03c3, 0x03c2, 0x03ac, 0x0436, 0x0561, 0x0587, 0x3042, 0xff41};
	static const WCHAR scriptsUpper[] = {0x03a3, 0x03c2, 0x0386, 0x0416, 0x0531, 0x0587, 0x3042, 0xff21};
	static const WCHAR surrogateUnits[] = {0xd800, 'a', 0xdc00, 0xdbff, 'b', 0xdfff, 0xffff, 0xe000};
	static const WCHAR surrogateUpper[] = {0xd800, 'A', 0xdc00, 0xdbff, 'B', 0xdfff, 0xffff, 0xe000};
	static const WCHAR pairs[] = {0xd801, 0xdc28, 0, 0xd83a, 0xdd22, 0xd801, 0xdd97, 'c'};
	static const WCHAR pairsUpper[] = {0xd801, 0xdc00, 0, 0xd83a, 0xdd00, 0xd801, 0xdd70, 'C'};
	checkBuffer(upper, "zero", ascii, asciiUpper, sizeof(ascii) / sizeof(ascii[0]), 0);
	checkBuffer(upper, "partial", ascii, asciiUpper, sizeof(ascii) / sizeof(ascii[0]), 2);
	checkBuffer(upper, "embedded-null", ascii, asciiUpper, sizeof(ascii) / sizeof(ascii[0]), 8);
	checkBuffer(upper, "latin", latin, latinUpper, sizeof(latin) / sizeof(latin[0]), 8);
	checkBuffer(upper, "scripts", scripts, scriptsUpper, sizeof(scripts) / sizeof(scripts[0]), 8);
	checkBuffer(upper, "isolated-surrogates", surrogateUnits, surrogateUpper,
				sizeof(surrogateUnits) / sizeof(surrogateUnits[0]), 8);
	checkBuffer(upper, "paired-surrogates", pairs, pairsUpper, sizeof(pairs) / sizeof(pairs[0]), 8);
	checkBuffer(upper, "split-pair", pairs, pairsUpper, sizeof(pairs) / sizeof(pairs[0]), 1);
	checkAsciiDomain(upper);
	SetLastError(0x4321);
	const DWORD emptyResult = upper(NULL, 0);
	TEST_CHECK_EQ(0, emptyResult);
	TEST_CHECK_EQ(0x4321, GetLastError());
	printf("null-zero result=%lu error=%lu\n", (unsigned long)emptyResult, (unsigned long)GetLastError());
	if (modeLength) {
		static WCHAR large[16003];
		for (size_t index = 0; index < sizeof(large) / sizeof(large[0]); ++index)
			large[index] = 'a';
		large[8000] = 0x00e9;
		SetLastError(0x4321);
		TEST_CHECK_EQ(0, upper(large + 1, 16001));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		for (size_t index = 0; index < sizeof(large) / sizeof(large[0]); ++index)
			TEST_CHECK_EQ(index == 8000 ? 0x00e9 : 'a', large[index]);
		large[8000] = 'a';
		SetLastError(0x4321);
		TEST_CHECK_EQ(16001, upper(large + 1, 16001));
		TEST_CHECK_EQ(0x4321, GetLastError());
		TEST_CHECK_EQ('a', large[0]);
		TEST_CHECK_EQ('a', large[16002]);
		for (size_t index = 1; index <= 16001; ++index)
			TEST_CHECK_EQ('A', large[index]);
	}
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
