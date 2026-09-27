#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

typedef int(WINAPI *CompareStringFn)(LPCWSTR, DWORD, LPCWCH, int, LPCWCH, int, LPNLSVERSIONINFO, LPVOID, LPARAM);

struct CallResult {
	int order;
	DWORD error;
};

_Static_assert(sizeof(WCHAR) == 2, "Comparison operates on UTF-16 code units");

static CompareStringFn compare_string;

static struct CallResult call_comparison(const char *name, LPCWSTR locale, DWORD flags, LPCWCH left, int left_count,
										 LPCWCH right, int right_count, LPNLSVERSIONINFO version, LPVOID reserved,
										 LPARAM handle) {
	SetLastError(0x4321);
	struct CallResult result;
	result.order = compare_string(locale, flags, left, left_count, right, right_count, version, reserved, handle);
	result.error = GetLastError();
	printf("%s: flags=0x%lx left=%d right=%d result=%d error=%lu\n", name, (unsigned long)flags, left_count,
		   right_count, result.order, (unsigned long)result.error);
	return result;
}

static void check_order(struct CallResult result, int expected) {
	TEST_CHECK_EQ(expected, result.order);
	TEST_CHECK_EQ(0x4321, result.error);
}

static void check_failure(struct CallResult result, DWORD expected_error) {
	TEST_CHECK_EQ(0, result.order);
	TEST_CHECK_EQ(expected_error, result.error);
}

static void check_comparison(const char *name, LPCWSTR locale, DWORD flags, LPCWCH left, int left_count, LPCWCH right,
							 int right_count, int expected) {
	check_order(call_comparison(name, locale, flags, left, left_count, right, right_count, NULL, NULL, 0), expected);
}

static void test_comparisons(void) {
	check_comparison("less", L"en-US", 0, L"a", -1, L"b", -1, CSTR_LESS_THAN);
	check_comparison("greater", L"en-US", 0, L"b", -1, L"a", -1, CSTR_GREATER_THAN);
	check_comparison("equal", LOCALE_NAME_INVARIANT, 0, L"aB", 2, L"aB", 2, CSTR_EQUAL);
	struct CallResult sensitive = call_comparison("case-sensitive", L"en-US", 0, L"aB", 2, L"AB", 2, NULL, NULL, 0);
	TEST_CHECK(sensitive.order == CSTR_LESS_THAN || sensitive.order == CSTR_GREATER_THAN);
	TEST_CHECK_EQ(0x4321, sensitive.error);
	check_comparison("case-insensitive", L"en-US", NORM_IGNORECASE, L"aB", 2, L"AB", 2, CSTR_EQUAL);
	check_comparison("negative-two", L"en-US", 0, L"ab", -2, L"ac", -1, CSTR_LESS_THAN);
	check_comparison("zero-both", LOCALE_NAME_INVARIANT, 0, L"a", 0, L"b", 0, CSTR_EQUAL);
	check_comparison("zero-left", LOCALE_NAME_INVARIANT, 0, L"a", 0, L"b", 1, CSTR_LESS_THAN);
	check_comparison("zero-right", LOCALE_NAME_INVARIANT, 0, L"a", 1, L"b", 0, CSTR_GREATER_THAN);
	const WCHAR embedded_left[] = {'a', 0, 'b'};
	const WCHAR embedded_right[] = {'a', 0, 'c'};
	check_comparison("embedded-zero-suffix", L"en-US", 0, embedded_left, 3, embedded_right, 3, CSTR_LESS_THAN);
	check_comparison("explicit-prefix", L"en-US", 0, embedded_left, 1, embedded_right, 1, CSTR_EQUAL);
	const WCHAR dotless_i[] = {0x0131};
	const DWORD linguistic = LINGUISTIC_IGNORECASE | NORM_LINGUISTIC_CASING;
	check_comparison("turkish-linguistic", L"tr-TR", linguistic, L"I", 1, dotless_i, 1, CSTR_EQUAL);
	check_comparison("digits-lexical", L"en-US", 0, L"2", 1, L"10", 2, CSTR_GREATER_THAN);
	check_comparison("digits-numeric", L"en-US", SORT_DIGITSASNUMBERS, L"2", 1, L"10", 2, CSTR_LESS_THAN);
	// Reflexivity does not depend on a particular sort-table version.
	const WCHAR supplementary[] = {0xd801, 0xdc28};
	const WCHAR unpaired[] = {0xd800};
	check_comparison("supplementary", LOCALE_NAME_INVARIANT, 0, supplementary, 2, supplementary, 2, CSTR_EQUAL);
	check_comparison("unpaired", LOCALE_NAME_INVARIANT, 0, unpaired, 1, unpaired, 1, CSTR_EQUAL);
}

static void test_errors(void) {
	check_failure(call_comparison("null-left", L"en-US", 0, NULL, 0, L"a", 1, NULL, NULL, 0), ERROR_INVALID_PARAMETER);
	check_failure(call_comparison("null-right", L"en-US", 0, L"a", 1, NULL, 0, NULL, NULL, 0), ERROR_INVALID_PARAMETER);
	check_failure(call_comparison("invalid-flags", L"en-US", 0x80000000, L"a", 1, L"b", 1, NULL, NULL, 0),
				  ERROR_INVALID_FLAGS);
	check_failure(call_comparison("invalid-locale", L"invalid-fixture-locale", 0, L"a", 1, L"b", 1, NULL, NULL, 0),
				  ERROR_INVALID_PARAMETER);
}

static void test_unsupported(void) {
	NLSVERSIONINFO version = {0};
	version.dwNLSVersionInfoSize = sizeof(version);
	check_failure(call_comparison("unsupported-version", L"en-US", 0, L"a", 1, L"b", 1, &version, NULL, 0),
				  ERROR_NOT_SUPPORTED);
	check_failure(call_comparison("unsupported-reserved", L"en-US", 0, L"a", 1, L"b", 1, NULL, &version, 0),
				  ERROR_NOT_SUPPORTED);
	check_failure(call_comparison("unsupported-handle", L"en-US", 0, L"a", 1, L"b", 1, NULL, NULL, 1),
				  ERROR_NOT_SUPPORTED);
	check_failure(call_comparison("null-left", L"en-US", 0, NULL, 0, L"a", 1, NULL, NULL, 0), ERROR_INVALID_PARAMETER);
	check_failure(call_comparison("null-right", L"en-US", 0, L"a", 1, NULL, 0, NULL, NULL, 0), ERROR_INVALID_PARAMETER);
}

static void test_transport(const char *mode) {
	struct CallResult result =
		call_comparison("transport", LOCALE_NAME_INVARIANT, NORM_IGNORECASE, L"aB", 2, L"AB", 2, NULL, NULL, 0);
	if (strcmp(mode, "success") == 0) {
		check_order(result, CSTR_EQUAL);
	} else {
		const DWORD error = strcmp(mode, "failed") == 0		   ? ERROR_INVALID_PARAMETER
							: strcmp(mode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED
															   : ERROR_INVALID_DATA;
		check_failure(result, error);
	}
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "CompareStringEx");
	_Static_assert(sizeof(exported) == sizeof(compare_string), "Resolved function pointers have the same width");
	memcpy(&compare_string, &exported, sizeof(compare_string));
	TEST_CHECK(compare_string != NULL);
	const char *transport_mode = getenv("WIBO_FIXTURE_COMPARE_RESPONSE");
	if (transport_mode) {
		test_transport(transport_mode);
	} else if (getenv("WIBO_EXPECT_COMPARE_UNSUPPORTED")) {
		test_unsupported();
	} else {
		test_comparisons();
		test_errors();
	}
	return 0;
}
