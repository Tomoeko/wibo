#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef int(__cdecl *WidePrintFunction)(LPWSTR, LPCWSTR, ...);

typedef struct {
	DWORD before[4];
	WCHAR output[1100];
	DWORD after[4];
} PrintBuffer;

static void resetBuffer(PrintBuffer *buffer) { memset(buffer, 0xa5, sizeof(*buffer)); }

static void observe(const char *name, const PrintBuffer *buffer, int result, DWORD error) {
	TEST_CHECK_EQ(0x4321u, error);
	for (unsigned int index = 0; index < 4; ++index) {
		TEST_CHECK_EQ(0xa5a5a5a5u, buffer->before[index]);
		TEST_CHECK_EQ(0xa5a5a5a5u, buffer->after[index]);
	}
	for (unsigned int index = 1024; index < 1100; ++index)
		TEST_CHECK_EQ(0xa5a5, buffer->output[index]);
	size_t terminated = 0;
	while (terminated < 1100 && buffer->output[terminated] != 0)
		++terminated;
	TEST_CHECK(terminated < 1100);
	printf("%s result=%d firstNul=%u error=%lu data=", name, result, (unsigned int)terminated, (unsigned long)error);
	const size_t displayed = result >= 0 && result < 40 ? (size_t)result + 1 : 40;
	for (size_t index = 0; index < displayed; ++index)
		printf("%04x", (unsigned int)buffer->output[index]);
	printf(" tail=%04x%04x%04x%04x\n", (unsigned int)buffer->output[1020], (unsigned int)buffer->output[1021],
		   (unsigned int)buffer->output[1022], (unsigned int)buffer->output[1023]);
}

#define RUN(name, expected, format, ...)                                                                               \
	do {                                                                                                               \
		resetBuffer(&buffer);                                                                                          \
		SetLastError(0x4321);                                                                                          \
		const int result = formatWide(buffer.output, format, ##__VA_ARGS__);                                           \
		const DWORD error = GetLastError();                                                                            \
		observe(name, &buffer, result, error);                                                                         \
		TEST_CHECK_EQ(sizeof(expected) / sizeof(WCHAR) - 1, result);                                                   \
		TEST_CHECK(memcmp(buffer.output, expected, sizeof(expected)) == 0);                                            \
		TEST_CHECK_EQ(0xa5a5, buffer.output[result + 1]);                                                              \
	} while (0)

int main(void) {
	HMODULE module = LoadLibraryA("user32.dll");
	TEST_CHECK(module != NULL);
	FARPROC procedure = GetProcAddress(module, "wsprintfW");
	TEST_CHECK(procedure != NULL);
	WidePrintFunction formatWide;
	_Static_assert(sizeof(formatWide) == sizeof(procedure), "Function pointer width");
	memcpy(&formatWide, &procedure, sizeof(formatWide));
	PrintBuffer buffer;

	RUN("literal", L"wide \x4e2d\xd83d\xde00 %", L"wide \x4e2d\xd83d\xde00 %%");
	RUN("integers", L"-2147483648|-17|4294967295|1234abcd|1234ABCD", L"%d|%i|%u|%x|%X", INT32_MIN, -17, UINT32_MAX,
		0x1234abcdU, 0x1234abcdU);
	RUN("long-short", L"-2147483647|4294967295|305463295|305419896", L"%ld|%lu|%hd|%hu", (LONG)-2147483647,
		(ULONG)UINT32_MAX, 0x1234ffff, 0x12345678U);
	RUN("padding", L"-0000007|-7      |-007|    -007", L"%08d|%-08d|%.4d|%8.4d", -7, -7, -7, -7);
	RUN("hex-prefix", L"0x00001234|0X1234|0x0|00001234", L"%#08x|%#X|%#x|%08X", 0x1234U, 0x1234U, 0U, 0x1234U);
	RUN("strings", L"\x4e2d\xd83d\xde00|ascii|short|long|wide", L"%s|%S|%hs|%lS|%ws", L"\x4e2d\xd83d\xde00", "ascii",
		"short", L"long", L"wide");
	RUN("string-width", L"     abc|abc     |   ab|abcdef", L"%8.3s|%-8.3s|%05s|%.0s", L"abcdef", L"abcdef", L"ab",
		L"abcdef");
	RUN("characters", L"\x4e2d|A|B|\x03a9", L"%c|%C|%hc|%lC", 0x4e2d, 'A', 'B', 0x03a9);
	RUN("zero-characters", L"\0X\0Y\0Z", L"%cX%hcY%lcZ", 0, 0, 0);
	RUN("null-strings", L"(null)|(null)", L"%s|%S", (LPCWSTR)NULL, (LPCSTR)NULL);
	RUN("wide-integers", L"-9223372036854775808|18446744073709551615|123456789ABCDEF0", L"%I64d|%I64u|%I64X", INT64_MIN,
		UINT64_MAX, UINT64_C(0x123456789abcdef0));
#ifdef _WIN64
	const ULONG_PTR pointer = UINT64_C(0x123456789abcdef0);
#else
	const ULONG_PTR pointer = UINT32_C(0x89abcdef);
#endif
#ifdef _WIN64
	RUN("pointers", L"123456789ABCDEF0|123456789abcdef0|123456789ABCDEF0", L"%p|%Ix|%IX", (PVOID)pointer, pointer,
		pointer);
#else
	RUN("pointers", L"89ABCDEF|89abcdef|89ABCDEF", L"%p|%Ix|%IX", (PVOID)pointer, pointer, pointer);
#endif
	RUN("many-arguments", L"1:2:3:4:5:6:7:8:9:10", L"%u:%u:%u:%u:%u:%u:%u:%u:%u:%u", 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U,
		10U);
	RUN("mixed-slots", L"1/2/123456789abcdef0/\x4e2d/5", L"%u/%u/%I64x/%s/%u", 1U, 2U, UINT64_C(0x123456789abcdef0),
		L"\x4e2d", 5U);
	RUN("numeric-precision-zero", L"0|0|0|0", L"%.0d|%.d|%.0x|%.0u", 0, 0, 0U, 0U);
	RUN("wrapped-width", L"1| 1", L"%4294967296d|%4294967298d", 1, 1);
	RUN("wrapped-precision", L"1|01", L"%.4294967296d|%.4294967298d", 1, 1);
	RUN("unknown-formats", L"q|o|f|n", L"%q|%o|%f|%n", 17, 18, 19, 20);
	RUN("unknown-width", L"          q|          q|q", L"%10q|%010q|%-5q");
	RUN("unknown-precision", L"     q|q|     q", L"%8.3q|%.3q|%#5q");
	const UINT codePage = GetACP();
	TEST_CHECK(codePage == 1252 || codePage == 28591);
	const char narrow[] = {'x', (char)0x80, (char)0x82, (char)0xa0, (char)0xe9, (char)0xff, 0};
	if (codePage == 1252) {
		RUN("ansi-characters", L"\x20ac|\x201a|\x00a0|\x00e9|\x00ff", L"%C|%hC|%hc|%C|%C", 0x80, 0x82, 0xa0, 0xe9,
			0xff);
		RUN("ansi-string", L"x\x20ac\x201a\x00a0\x00e9\x00ff", L"%S", narrow);
		RUN("ansi-string-width", L"       x\x20ac\x201a|x\x20ac\x201a       ", L"%10.3S|%-10.3hs", narrow, narrow);
	} else {
		RUN("ansi-characters", L"\x0080|\x0082|\x00a0|\x00e9|\x00ff", L"%C|%hC|%hc|%C|%C", 0x80, 0x82, 0xa0, 0xe9,
			0xff);
		RUN("ansi-string", L"x\x0080\x0082\x00a0\x00e9\x00ff", L"%S", narrow);
		RUN("ansi-string-width", L"       x\x0080\x0082|x\x0080\x0082       ", L"%10.3S|%-10.3hs", narrow, narrow);
	}
	for (unsigned int value = 0; value < 256; ++value) {
		const char byte = (char)value;
		WCHAR converted = 0xa5a5;
		TEST_CHECK_EQ(1, MultiByteToWideChar(28591, 0, &byte, 1, &converted, 1));
		TEST_CHECK_EQ(value, converted);
		WCHAR currentPage = 0xa5a5;
		TEST_CHECK_EQ(1, MultiByteToWideChar(codePage, 0, &byte, 1, &currentPage, 1));
		resetBuffer(&buffer);
		SetLastError(0x4321);
		TEST_CHECK_EQ(1, formatWide(buffer.output, L"%C", value));
		TEST_CHECK_EQ(0x4321u, GetLastError());
		TEST_CHECK_EQ(currentPage, buffer.output[0]);
		TEST_CHECK_EQ(0, buffer.output[1]);
		TEST_CHECK_EQ(0xa5a5, buffer.output[2]);
	}

	WCHAR literal[1026];
	for (unsigned int index = 0; index < 1025; ++index)
		literal[index] = L'v';
	for (unsigned int length = 1022; length <= 1024; ++length) {
		literal[length] = 0;
		resetBuffer(&buffer);
		SetLastError(0x4321);
		const int result = formatWide(buffer.output, literal);
		observe("output-boundary", &buffer, result, GetLastError());
		const unsigned int written = length < 1023 ? length : 1023;
		TEST_CHECK_EQ(length < 1023 ? (int)length : 1024, result);
		for (unsigned int index = 0; index < written; ++index)
			TEST_CHECK_EQ(L'v', buffer.output[index]);
		TEST_CHECK_EQ(0, buffer.output[written]);
		TEST_CHECK_EQ(0xa5a5, buffer.output[written + 1]);
		literal[length] = L'v';
	}
	literal[1025] = 0;
	resetBuffer(&buffer);
	SetLastError(0x4321);
	const int result = formatWide(buffer.output, L"%s", literal);
	observe("formatted-boundary", &buffer, result, GetLastError());
	TEST_CHECK_EQ(1024, result);
	for (unsigned int index = 0; index < 1023; ++index)
		TEST_CHECK_EQ(L'v', buffer.output[index]);
	TEST_CHECK_EQ(0, buffer.output[1023]);
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
