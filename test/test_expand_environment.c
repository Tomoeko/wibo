#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

// https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-expandenvironmentstringsa
// https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-expandenvironmentstringsw
// All source strings and non-NULL destinations are valid independent buffers.
// Current-ACP variables are ASCII; wide literals retain all UTF-16 code units.
static const DWORD kSentinelError = 0x12345678;

static void check_expansion(const char *source, const char *expected) {
	char output[256];
	WCHAR wide_source[256], wide_expected[256], wide_output[256];
	size_t length = strlen(expected);
	for (size_t i = 0; i <= strlen(source); ++i)
		wide_source[i] = (unsigned char)source[i];
	for (size_t i = 0; i <= length; ++i)
		wide_expected[i] = (unsigned char)expected[i];
	SetLastError(kSentinelError);
	// Microsoft's ANSI buffer rule requires one extra character. Wine returns
	// this larger requirement for queries/short buffers, but length+1 on success.
	TEST_CHECK_EQ(length + 2, ExpandEnvironmentStringsA(source, NULL, 0));
	TEST_CHECK_EQ(length + 1, ExpandEnvironmentStringsW(wide_source, NULL, 0));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	memset(output, '!', sizeof(output));
	TEST_CHECK_EQ(length + 2, ExpandEnvironmentStringsA(source, output, (DWORD)length + 1));
	TEST_CHECK_EQ(0, output[0]);
	TEST_CHECK_EQ('!', output[length + 1]);
	memset(output, '!', sizeof(output));
	TEST_CHECK_EQ(length + 1, ExpandEnvironmentStringsA(source, output, (DWORD)length + 2));
	TEST_CHECK_STR_EQ(expected, output);
	TEST_CHECK_EQ('!', output[length + 1]);
	for (unsigned i = 0; i < 256; ++i)
		wide_output[i] = '!';
	TEST_CHECK_EQ(length + 1, ExpandEnvironmentStringsW(wide_source, wide_output, (DWORD)length + 1));
	TEST_CHECK_EQ(0, wcscmp(wide_expected, wide_output));
	TEST_CHECK_EQ('!', wide_output[length + 1]);
	TEST_CHECK_EQ(kSentinelError, GetLastError());
}

static void test_substitution(void) {
	TEST_CHECK(SetEnvironmentVariableA("WIBO_EXPAND_ONE", "abc"));
	TEST_CHECK(SetEnvironmentVariableA("WIBO_EXPAND_TWO", "DEF"));
	TEST_CHECK(SetEnvironmentVariableA("WIBO_EXPAND_EMPTY", ""));
	TEST_CHECK(SetEnvironmentVariableA("WIBO_EXPAND_CHAIN", "%WIBO_EXPAND_ONE%"));
	TEST_CHECK(SetEnvironmentVariableA("WIBO_EXPAND_UNKNOWN", NULL));
	check_expansion("", "");
	check_expansion("plain text", "plain text");
	check_expansion("%wIbO_eXpAnD_oNe%", "abc");
	check_expansion("x%WIBO_EXPAND_ONE%y", "xabcy");
	check_expansion("%WIBO_EXPAND_ONE%%WIBO_EXPAND_TWO%", "abcDEF");
	check_expansion("a%WIBO_EXPAND_EMPTY%b", "ab");
	check_expansion("%WIBO_EXPAND_EMPTY%", "");
	check_expansion("%WIBO_EXPAND_UNKNOWN%", "%WIBO_EXPAND_UNKNOWN%");
	check_expansion("unmatched%WIBO_EXPAND_ONE", "unmatched%WIBO_EXPAND_ONE");
	check_expansion("%%", "%%");
	check_expansion("%%%WIBO_EXPAND_ONE%", "%%abc");
	check_expansion("%WIBO_EXPAND_CHAIN%", "%WIBO_EXPAND_ONE%");
	check_expansion("%WIBO_EXPAND_ONE:~1,2%", "%WIBO_EXPAND_ONE:~1,2%");
	check_expansion("%WIBO_EXPAND_ONE:a=z%", "%WIBO_EXPAND_ONE:a=z%");
}

static void test_short_buffers(void) {
	static const WCHAR expected[8][8] = {
		{'!', '!', '!', '!', '!', '!', '!', '!'}, {'!', '!', '!', '!', '!', '!', '!', '!'},
		{'x', 0, '!', '!', '!', '!', '!', '!'},	  {'x', 0, '!', '!', '!', '!', '!', '!'},
		{'x', 0, '!', '!', '!', '!', '!', '!'},	  {'x', 'a', 'b', 'c', 0, '!', '!', '!'},
		{'x', 'a', 'b', 'c', 'y', 0, '!', '!'},	  {'x', 'a', 'b', 'c', 'y', 0, '!', '!'},
	};
	for (DWORD size = 0; size < 8; ++size) {
		char output[8];
		WCHAR wide_output[8];
		memset(output, '!', sizeof(output));
		for (unsigned i = 0; i < 8; ++i)
			wide_output[i] = '!';
		SetLastError(kSentinelError);
		TEST_CHECK_EQ(size < 7 ? 7 : 6, ExpandEnvironmentStringsA("x%WIBO_EXPAND_ONE%y", output, size));
		if (size < 7) {
			TEST_CHECK_EQ(0, output[0]);
			for (unsigned i = 1; i < 8; ++i)
				TEST_CHECK_EQ('!', output[i]);
		} else {
			TEST_CHECK_STR_EQ("xabcy", output);
			TEST_CHECK_EQ('!', output[6]);
		}
		TEST_CHECK_EQ(6, ExpandEnvironmentStringsW(L"x%WIBO_EXPAND_ONE%y", wide_output, size));
		TEST_CHECK_EQ(0, memcmp(expected[size], wide_output, sizeof(wide_output)));
		TEST_CHECK_EQ(kSentinelError, GetLastError());
	}
	WCHAR literal[5] = {'!', '!', '!', '!', '!'};
	TEST_CHECK_EQ(5, ExpandEnvironmentStringsW(L"abcd", literal, 3));
	TEST_CHECK_EQ('a', literal[0]);
	TEST_CHECK_EQ('b', literal[1]);
	TEST_CHECK_EQ('!', literal[2]);
	TEST_CHECK_EQ('!', literal[3]);
}

static void test_wide_literals_and_names(void) {
	static const WCHAR source[] = {0x4e2d, '%', 'W', 'I', 'B', 'O', '_', 'E',	 'X',	 'P', 'A',
								   'N',	   'D', '_', 'O', 'N', 'E', '%', 0xd83d, 0xde00, 0};
	static const WCHAR expected[] = {0x4e2d, 'a', 'b', 'c', 0xd83d, 0xde00, 0};
	WCHAR output[128];
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(7, ExpandEnvironmentStringsW(source, output, 128));
	TEST_CHECK_EQ(0, wcscmp(expected, output));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK(SetEnvironmentVariableA("WIBO_EXPAND_A", "must not alias"));
	const WCHAR *unknown = L"%WIBO_EXPAND_\x0141%";
	TEST_CHECK_EQ(wcslen(unknown) + 1, ExpandEnvironmentStringsW(unknown, output, 128));
	TEST_CHECK_EQ(0, wcscmp(unknown, output));
}

static void test_path_environment_consistency(void) {
	static const char *names[] = {"PATH", "TMP", "TEMP"};
	static const char *sources[] = {"%PATH%", "%TMP%", "%TEMP%"};
	char value[16384], output[16386];
	for (unsigned i = 0; i < 3; ++i) {
		DWORD length = GetEnvironmentVariableA(names[i], value, sizeof(value));
		TEST_CHECK(length > 0 && length < sizeof(value));
		TEST_CHECK_EQ(length + 1, ExpandEnvironmentStringsA(sources[i], output, sizeof(output)));
		TEST_CHECK_STR_EQ(value, output);
	}
}

int main(void) {
	test_substitution();
	test_short_buffers();
	test_wide_literals_and_names();
	test_path_environment_consistency();
	puts("environment expansion tests passed");
	return 0;
}
