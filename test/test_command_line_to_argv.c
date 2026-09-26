#include "test_assert.h"
#include <wchar.h>
#include <windows.h>

#include <shellapi.h>

static void check_arguments(const WCHAR *command, int expected_count, const WCHAR **expected) {
	int count = -1;
	WCHAR **arguments = CommandLineToArgvW(command, &count);
	TEST_CHECK(arguments != NULL);
	TEST_CHECK_EQ(expected_count, count);
	for (int i = 0; i < count; ++i) {
		TEST_CHECK(wcscmp(arguments[i], expected[i]) == 0);
	}
	TEST_CHECK(LocalFree(arguments) == NULL);
}

int main(void) {
	const WCHAR *quoted[] = {L"C:\\folder path\\tool.exe", L"a b", L"", L"tail"};
	check_arguments(L"\"C:\\folder path\\tool.exe\" \"a b\" \"\" tail  ", 4, quoted);
	const WCHAR *leading[] = {L"", L"first", L"second"};
	check_arguments(L" \t first second", 3, leading);
	const WCHAR *escaped[] = {L"tool", L"a\"b", L"a\\b c", L"end\\\\"};
	check_arguments(L"tool a\\\"b a\\b\" c\" end\\\\", 4, escaped);
	WCHAR path[32768];
	TEST_CHECK(GetModuleFileNameW(NULL, path, 32768) > 0);
	const WCHAR *empty[] = {path};
	check_arguments(L"", 1, empty);
	return 0;
}
