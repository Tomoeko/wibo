#include <stdarg.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef int(__cdecl *FormatFn)(char *buffer, size_t count, const char *format, va_list arguments);

static FormatFn format_output;

static int format(char *buffer, size_t count, const char *pattern, ...) {
	va_list arguments;
	va_start(arguments, pattern);
	int result = format_output(buffer, count, pattern, arguments);
	va_end(arguments);
	return result;
}

int main(void) {
	HMODULE module = GetModuleHandleW(L"ntdll.dll");
	TEST_CHECK(module != NULL);
	FARPROC procedure = GetProcAddress(module, "_vsnprintf");
	TEST_CHECK(procedure != NULL);
	TEST_CHECK(sizeof(format_output) == sizeof(procedure));
	memcpy(&format_output, &procedure, sizeof(format_output));

	char buffer[80];
	memset(buffer, '#', sizeof(buffer));
	const char *expected = "sample -37 123456789abcdef0";
	TEST_CHECK_EQ((int)strlen(expected),
				  format(buffer, sizeof(buffer), "%s %d %I64x", "sample", -37, 0x123456789abcdef0ULL));
	TEST_CHECK(strcmp(buffer, expected) == 0);

	memset(buffer, '#', sizeof(buffer));
	TEST_CHECK_EQ(-1, format(buffer, 5, "%s", "abcdef"));
	TEST_CHECK(memcmp(buffer, "abcde#", 6) == 0);

	memset(buffer, '#', sizeof(buffer));
	TEST_CHECK_EQ(5, format(buffer, 5, "%s", "abcde"));
	TEST_CHECK(memcmp(buffer, "abcde#", 6) == 0);

	memset(buffer, '#', sizeof(buffer));
	TEST_CHECK_EQ(4, format(buffer, 5, "%s", "abcd"));
	TEST_CHECK(memcmp(buffer, "abcd\0#", 6) == 0);

	memset(buffer, '#', sizeof(buffer));
	TEST_CHECK_EQ(-1, format(buffer, 0, "%s", "abc"));
	TEST_CHECK(buffer[0] == '#');
	return 0;
}
