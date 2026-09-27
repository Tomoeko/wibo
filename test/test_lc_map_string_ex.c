#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

#include <stddef.h>

typedef int(WINAPI *MapStringFn)(LPCWSTR, DWORD, LPCWSTR, int, LPWSTR, int, LPNLSVERSIONINFO, LPVOID, LPARAM);

enum { output_units = 128 };

struct GuardedOutput {
	BYTE before[16];
	WCHAR text[output_units];
	BYTE after[16];
};

struct CallResult {
	int count;
	DWORD error;
};

_Static_assert(sizeof(WCHAR) == 2, "Mapping operates on UTF-16 code units");
_Static_assert(offsetof(struct GuardedOutput, text) == 16, "The output follows its leading canary");

static MapStringFn map_string;

static void reset_output(struct GuardedOutput *output) { memset(output, 0xa5, sizeof(*output)); }

static void check_output_bounds(const struct GuardedOutput *output, size_t capacity_bytes) {
	TEST_CHECK(capacity_bytes <= sizeof(output->text));
	for (unsigned index = 0; index < sizeof(output->before); ++index) {
		TEST_CHECK_EQ(0xa5, output->before[index]);
		TEST_CHECK_EQ(0xa5, output->after[index]);
	}
	const BYTE *bytes = (const BYTE *)output->text;
	for (size_t index = capacity_bytes; index < sizeof(output->text); ++index) {
		TEST_CHECK_EQ(0xa5, bytes[index]);
	}
}

static struct CallResult call_mapping(const char *name, LPCWSTR locale, DWORD flags, LPCWSTR source, int source_count,
									  LPWSTR destination, int capacity, LPNLSVERSIONINFO version, LPVOID reserved,
									  LPARAM handle) {
	SetLastError(0x4321);
	struct CallResult result;
	result.count = map_string(locale, flags, source, source_count, destination, capacity, version, reserved, handle);
	result.error = GetLastError();
	printf("%s: flags=0x%lx source=%d capacity=%d result=%d error=%lu\n", name, (unsigned long)flags, source_count,
		   capacity, result.count, (unsigned long)result.error);
	return result;
}

static void check_success(struct CallResult result, int expected_count) {
	TEST_CHECK_EQ(expected_count, result.count);
	TEST_CHECK_EQ(0x4321, result.error);
}

static void check_failure(struct CallResult result, DWORD expected_error) {
	TEST_CHECK_EQ(0, result.count);
	TEST_CHECK_EQ(expected_error, result.error);
}

static void check_mapping(const char *name, LPCWSTR locale, DWORD flags, LPCWSTR source, int source_count,
						  const WCHAR *expected, int expected_count) {
	TEST_CHECK(expected_count > 0 && expected_count <= output_units);
	struct GuardedOutput output;
	check_success(call_mapping(name, locale, flags, source, source_count, NULL, 0, NULL, NULL, 0), expected_count);
	reset_output(&output);
	check_success(call_mapping(name, locale, flags, source, source_count, output.text, 0, NULL, NULL, 0),
				  expected_count);
	check_output_bounds(&output, 0);
	check_success(call_mapping(name, locale, flags, source, source_count, output.text, expected_count, NULL, NULL, 0),
				  expected_count);
	TEST_CHECK(memcmp(expected, output.text, expected_count * sizeof(WCHAR)) == 0);
	check_output_bounds(&output, expected_count * sizeof(WCHAR));
	if (expected_count > 1) {
		reset_output(&output);
		check_failure(
			call_mapping(name, locale, flags, source, source_count, output.text, expected_count - 1, NULL, NULL, 0),
			ERROR_INSUFFICIENT_BUFFER);
		// Partial output is permitted on failure; bytes outside the supplied capacity are not.
		check_output_bounds(&output, (expected_count - 1) * sizeof(WCHAR));
	}
}

static void test_casing(void) {
	check_mapping("upper-negative-one", L"en-US", LCMAP_UPPERCASE, L"aB", -1, L"AB", 3);
	check_mapping("upper-negative-two", L"en-US", LCMAP_UPPERCASE, L"aB", -2, L"AB", 3);
	check_mapping("lower-invariant", LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, L"Ab", -1, L"ab", 3);
	check_mapping("upper-user-default", LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, L"aB", -1, L"AB", 3);
	check_mapping("upper-system-default", LOCALE_NAME_SYSTEM_DEFAULT, LCMAP_UPPERCASE, L"aB", -1, L"AB", 3);
	const WCHAR embedded_source[] = {'a', 0, 'b'};
	const WCHAR embedded_expected[] = {'A', 0, 'B'};
	check_mapping("upper-embedded-zero", L"en-US", LCMAP_UPPERCASE, embedded_source, 3, embedded_expected, 3);
	const WCHAR dotted_i[] = {0x0130};
	const WCHAR dotless_i[] = {0x0131};
	check_mapping("upper-turkish", L"tr-TR", LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, L"i", 1, dotted_i, 1);
	check_mapping("lower-turkish", L"tr-TR", LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, L"I", 1, dotless_i, 1);

	struct GuardedOutput inplace;
	reset_output(&inplace);
	memcpy(inplace.text, L"aB", 3 * sizeof(WCHAR));
	check_success(
		call_mapping("upper-inplace", L"en-US", LCMAP_UPPERCASE, inplace.text, -1, inplace.text, 3, NULL, NULL, 0), 3);
	TEST_CHECK(memcmp(inplace.text, L"AB", 3 * sizeof(WCHAR)) == 0);
	check_output_bounds(&inplace, 3 * sizeof(WCHAR));
	reset_output(&inplace);
	memcpy(inplace.text, L"aB", 3 * sizeof(WCHAR));
	check_success(call_mapping("upper-inplace-explicit", L"en-US", LCMAP_UPPERCASE, inplace.text, 2, inplace.text, 3,
							   NULL, NULL, 0),
				  2);
	TEST_CHECK(memcmp(inplace.text, L"AB", 2 * sizeof(WCHAR)) == 0);
	TEST_CHECK_EQ(0, inplace.text[2]);
	check_output_bounds(&inplace, 3 * sizeof(WCHAR));
	reset_output(&inplace);
	check_success(
		call_mapping("upper-extra-capacity", L"en-US", LCMAP_UPPERCASE, L"aB", 2, inplace.text, 8, NULL, NULL, 0), 2);
	TEST_CHECK(memcmp(inplace.text, L"AB", 2 * sizeof(WCHAR)) == 0);
	check_output_bounds(&inplace, 2 * sizeof(WCHAR));
}

static void test_sort_keys(void) {
	const DWORD flags = LCMAP_SORTKEY | NORM_IGNORECASE;
	struct CallResult query = call_mapping("sort-query", L"en-US", flags, L"aB", -1, NULL, 0, NULL, NULL, 0);
	TEST_CHECK(query.count > 1 && query.count <= (int)(output_units * sizeof(WCHAR)));
	TEST_CHECK_EQ(0x4321, query.error);
	struct GuardedOutput first, equivalent;
	reset_output(&first);
	reset_output(&equivalent);
	check_success(call_mapping("sort-query-buffer", L"en-US", flags, L"aB", -1, first.text, 0, NULL, NULL, 0),
				  query.count);
	check_output_bounds(&first, 0);
	check_success(call_mapping("sort-write", L"en-US", flags, L"aB", -1, first.text, query.count, NULL, NULL, 0),
				  query.count);
	check_output_bounds(&first, query.count);
	check_success(call_mapping("sort-equivalent-query", L"en-US", flags, L"Ab", -1, NULL, 0, NULL, NULL, 0),
				  query.count);
	check_success(
		call_mapping("sort-equivalent-write", L"en-US", flags, L"Ab", -1, equivalent.text, query.count, NULL, NULL, 0),
		query.count);
	TEST_CHECK(memcmp(first.text, equivalent.text, query.count) == 0);
	check_output_bounds(&equivalent, query.count);
	reset_output(&equivalent);
	check_success(call_mapping("sort-repeat", L"en-US", flags, L"aB", -1, equivalent.text, query.count, NULL, NULL, 0),
				  query.count);
	TEST_CHECK(memcmp(first.text, equivalent.text, query.count) == 0);
	reset_output(&equivalent);
	check_failure(
		call_mapping("sort-short", L"en-US", flags, L"aB", -1, equivalent.text, query.count - 1, NULL, NULL, 0),
		ERROR_INSUFFICIENT_BUFFER);
	check_output_bounds(&equivalent, query.count - 1);
}

static void test_errors(void) {
	struct GuardedOutput output;
	reset_output(&output);
	check_failure(call_mapping("null-source", L"en-US", LCMAP_UPPERCASE, NULL, 1, output.text, 8, NULL, NULL, 0),
				  ERROR_INVALID_PARAMETER);
	check_failure(call_mapping("empty-count", L"en-US", LCMAP_UPPERCASE, L"aB", 0, output.text, 8, NULL, NULL, 0),
				  ERROR_INVALID_PARAMETER);
	check_failure(
		call_mapping("negative-capacity", L"en-US", LCMAP_UPPERCASE, L"aB", 2, output.text, -1, NULL, NULL, 0),
		ERROR_INVALID_PARAMETER);
	check_failure(
		call_mapping("invalid-flags", L"en-US", LCMAP_UPPERCASE | 0x80000000, L"aB", 2, output.text, 8, NULL, NULL, 0),
		ERROR_INVALID_FLAGS);
	check_failure(call_mapping("invalid-locale", L"invalid-fixture-locale", LCMAP_UPPERCASE, L"aB", 2, output.text, 8,
							   NULL, NULL, 0),
				  ERROR_INVALID_PARAMETER);
	check_output_bounds(&output, 8 * sizeof(WCHAR));
}

static void observe_unicode(const char *name, const WCHAR *source, int source_count) {
	struct GuardedOutput output;
	reset_output(&output);
	struct CallResult result = call_mapping(name, LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, source, source_count,
											output.text, output_units, NULL, NULL, 0);
	TEST_CHECK(result.count >= 0 && result.count <= output_units);
	check_output_bounds(&output, sizeof(output.text));
	printf("%s code units:", name);
	for (int index = 0; index < result.count; ++index) {
		printf(" %04x", output.text[index]);
	}
	putchar('\n');
}

static void test_unsupported(void) {
	struct GuardedOutput output;
	reset_output(&output);
	NLSVERSIONINFO version = {0};
	version.dwNLSVersionInfoSize = sizeof(version);
	check_failure(
		call_mapping("unsupported-hash", L"en-US", LCMAP_HASH, L"aB", 2, output.text, output_units, NULL, NULL, 0),
		ERROR_NOT_SUPPORTED);
	check_failure(call_mapping("unsupported-sort-handle", L"en-US", LCMAP_SORTHANDLE, L"aB", 2, output.text,
							   output_units, NULL, NULL, 0),
				  ERROR_NOT_SUPPORTED);
	check_failure(call_mapping("unsupported-version", L"en-US", LCMAP_UPPERCASE, L"aB", 2, output.text, output_units,
							   &version, NULL, 0),
				  ERROR_NOT_SUPPORTED);
	check_failure(call_mapping("unsupported-reserved", L"en-US", LCMAP_UPPERCASE, L"aB", 2, output.text, output_units,
							   NULL, &version, 0),
				  ERROR_NOT_SUPPORTED);
	check_failure(call_mapping("unsupported-handle", L"en-US", LCMAP_UPPERCASE, L"aB", 2, output.text, output_units,
							   NULL, NULL, 1),
				  ERROR_NOT_SUPPORTED);
	check_output_bounds(&output, 0);
	check_failure(call_mapping("null-destination", L"en-US", LCMAP_UPPERCASE, L"aB", 2, NULL, 2, NULL, NULL, 0),
				  ERROR_INSUFFICIENT_BUFFER);
}

static void test_transport(const char *mode) {
	struct GuardedOutput output;
	reset_output(&output);
	const int large_query = strcmp(mode, "large-query") == 0;
	const int query = strcmp(mode, "query-data") == 0 || large_query;
	struct CallResult result = call_mapping("transport", LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, L"aB", 2, output.text,
											query ? 0 : 2, NULL, NULL, 0);
	if (large_query) {
		const int required_units = 8 * 1024 * 1024 + 1;
		check_success(result, required_units);
		check_output_bounds(&output, 0);
		WCHAR *large_output = malloc((size_t)required_units * sizeof(WCHAR));
		TEST_CHECK(large_output != NULL);
		large_output[0] = 0xa5a5;
		large_output[required_units - 1] = 0xa5a5;
		check_failure(call_mapping("transport-large-write", LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, L"aB", 2,
								   large_output, required_units, NULL, NULL, 0),
					  ERROR_NOT_ENOUGH_MEMORY);
		TEST_CHECK_EQ(0xa5a5, large_output[0]);
		TEST_CHECK_EQ(0xa5a5, large_output[required_units - 1]);
		free(large_output);
	} else if (strcmp(mode, "success") == 0) {
		check_success(result, 2);
		TEST_CHECK(memcmp(output.text, L"AB", 2 * sizeof(WCHAR)) == 0);
		check_output_bounds(&output, 2 * sizeof(WCHAR));
		reset_output(&output);
		check_success(call_mapping("transport-query", LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, L"aB", 2, output.text, 0,
								   NULL, NULL, 0),
					  2);
		check_output_bounds(&output, 0);
	} else {
		const DWORD error = strcmp(mode, "failed") == 0		   ? ERROR_INSUFFICIENT_BUFFER
							: strcmp(mode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED
															   : ERROR_INVALID_DATA;
		check_failure(result, error);
		check_output_bounds(&output, 0);
	}
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "LCMapStringEx");
	_Static_assert(sizeof(exported) == sizeof(map_string), "Resolved function pointers have the same width");
	memcpy(&map_string, &exported, sizeof(map_string));
	TEST_CHECK(map_string != NULL);
	const char *transport_mode = getenv("WIBO_FIXTURE_NLS_RESPONSE");
	if (transport_mode) {
		test_transport(transport_mode);
	} else if (getenv("WIBO_EXPECT_NLS_UNSUPPORTED")) {
		test_unsupported();
	} else {
		test_casing();
		test_sort_keys();
		test_errors();
		// Observe table-version-sensitive results without declaring Unicode parity.
		const WCHAR supplementary[] = {0xd801, 0xdc28};
		const WCHAR unpaired[] = {0xd800};
		observe_unicode("supplementary", supplementary, 2);
		observe_unicode("unpaired", unpaired, 1);
	}
	return 0;
}
