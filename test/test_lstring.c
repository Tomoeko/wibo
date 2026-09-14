#include <windows.h>

#include "test_assert.h"

// Contracts: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-lstrlena
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-lstrcmpa
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-lstrcmpiw
// Only lstrlen documents NULL as valid. Comparisons below use terminated,
// accessible strings. These cases do not establish complete NLS word sorting.
static const DWORD kSentinelError = 0x12345678;

static int sign(int value) { return (value > 0) - (value < 0); }

static void test_lengths(void) {
	static const WCHAR wide[] = {0x4e2d, 0xd83d, 0xde00, 0};
	static const char terminated[] = {'a', 'b', 0, 'c', 0};
	static const WCHAR wide_terminated[] = {'a', 'b', 0, 'c', 0};
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, lstrlenA(NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, lstrlenW(NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, lstrlenA(""));
	TEST_CHECK_EQ(0, lstrlenW(L""));
	TEST_CHECK_EQ(5, lstrlenA("hello"));
	TEST_CHECK_EQ(5, lstrlenW(L"hello"));
	TEST_CHECK_EQ(2, lstrlenA(terminated));
	TEST_CHECK_EQ(2, lstrlenW(wide_terminated));
	TEST_CHECK_EQ(3, lstrlenW(wide));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
}

static void test_ascii_comparisons(void) {
	struct Case {
		const char *left;
		const char *right;
		const WCHAR *wide_left;
		const WCHAR *wide_right;
		int sensitive;
		int insensitive;
	};
	static const struct Case cases[] = {
		{"", "", L"", L"", 0, 0},
		{"", "a", L"", L"a", -1, -1},
		{"a", "", L"a", L"", 1, 1},
		{"alpha", "alpha", L"alpha", L"alpha", 0, 0},
		{"alpha", "alphabeta", L"alpha", L"alphabeta", -1, -1},
		{"alphabeta", "alpha", L"alphabeta", L"alpha", 1, 1},
		{"abc", "abd", L"abc", L"abd", -1, -1},
		{"ABD", "ABC", L"ABD", L"ABC", 1, 1},
		{"123", "124", L"123", L"124", -1, -1},
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		const struct Case *item = &cases[i];
		SetLastError(kSentinelError);
		TEST_CHECK_EQ(item->sensitive, sign(lstrcmpA(item->left, item->right)));
		TEST_CHECK_EQ(item->sensitive, sign(lstrcmpW(item->wide_left, item->wide_right)));
		TEST_CHECK_EQ(item->insensitive, sign(lstrcmpiA(item->left, item->right)));
		TEST_CHECK_EQ(item->insensitive, sign(lstrcmpiW(item->wide_left, item->wide_right)));
		TEST_CHECK_EQ(kSentinelError, GetLastError());
	}
	TEST_CHECK_EQ(0, lstrcmpiA("MiXeD", "mixed"));
	TEST_CHECK_EQ(0, lstrcmpiW(L"MiXeD", L"mixed"));
	TEST_CHECK(lstrcmpA("MiXeD", "mixed") != 0);
	TEST_CHECK(lstrcmpW(L"MiXeD", L"mixed") != 0);
}

static void test_full_wide_units(void) {
	static const WCHAR first[] = {'x', 0x4e2d, 0};
	static const WCHAR second[] = {'x', 0x5e2d, 0};
	static const WCHAR copy[] = {'x', 0x4e2d, 0};
	static const WCHAR capital[] = {'X', 0x4e2d, 0};
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, lstrcmpW(first, copy));
	TEST_CHECK_EQ(0, lstrcmpiW(first, copy));
	TEST_CHECK_EQ(0, lstrcmpiW(first, capital));
	TEST_CHECK(lstrcmpW(first, second) != 0);
	TEST_CHECK(lstrcmpiW(first, second) != 0);
	TEST_CHECK_EQ(-sign(lstrcmpW(second, first)), sign(lstrcmpW(first, second)));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
}

int main(void) {
	test_lengths();
	test_ascii_comparisons();
	test_full_wide_units();
	// Expose inherited collation limits rather than making them passing claims.
	printf("NLS diagnostics (not acceptance): A/a=%d, coop/co-op=%d, e-acute case=%d\n", sign(lstrcmpA("A", "a")),
		   sign(lstrcmpA("coop", "co-op")), sign(lstrcmpiW(L"\x00e9", L"\x00c9")));
	puts("lstring defined-input tests passed");
	return 0;
}
