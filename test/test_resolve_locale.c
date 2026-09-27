#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

#include <wchar.h>

typedef int(WINAPI *ResolveLocaleFn)(LPCWSTR, LPWSTR, int);

enum { BUFFER_UNITS = LOCALE_NAME_MAX_LENGTH, BUFFER_PREFIX = 16 };

union GuardedBuffer {
	DWORD alignment;
	BYTE bytes[BUFFER_PREFIX + BUFFER_UNITS * sizeof(WCHAR) + 16];
};

struct CallResult {
	int result;
	DWORD error;
};

_Static_assert(sizeof(WCHAR) == 2, "Locale capacities count UTF16 units");

static ResolveLocaleFn resolve_locale;

static void initialize_buffer(union GuardedBuffer *buffer) { memset(buffer, 0xa5, sizeof(*buffer)); }

static BYTE *buffer_data(union GuardedBuffer *buffer) { return buffer->bytes + BUFFER_PREFIX; }

static struct CallResult call_resolve(const char *label, LPCWSTR name, int capacity, union GuardedBuffer *output) {
	TEST_CHECK(capacity <= BUFFER_UNITS);
	SetLastError(0x4321);
	struct CallResult result;
	result.result = resolve_locale(name, output ? (LPWSTR)buffer_data(output) : NULL, capacity);
	result.error = GetLastError();
	if (output) {
		size_t allowed = capacity < 0 ? BUFFER_UNITS * sizeof(WCHAR) : (size_t)capacity * sizeof(WCHAR);
		for (size_t index = 0; index < sizeof(output->bytes); ++index)
			if (index < BUFFER_PREFIX || index >= BUFFER_PREFIX + allowed)
				TEST_CHECK_EQ(0xa5, output->bytes[index]);
	}
	printf("%s: capacity=%d result=%d error=%lu\n", label, capacity, result.result, (unsigned long)result.error);
	return result;
}

static void check_text(union GuardedBuffer *output, int result, LPCWSTR expected) {
	TEST_CHECK(result > 0 && result <= BUFFER_UNITS);
	WCHAR text[BUFFER_UNITS];
	memcpy(text, buffer_data(output), (size_t)result * sizeof(WCHAR));
	TEST_CHECK_EQ(0, text[result - 1]);
	TEST_CHECK_EQ(result - 1, wcslen(text));
	if (expected)
		TEST_CHECK_EQ(0, wcscmp(expected, text));
	printf("resolved-units:");
	for (int index = 0; index < result; ++index)
		printf(" %04x", (unsigned)text[index]);
	printf("\n");
}

static void test_name(const char *label, LPCWSTR name, LPCWSTR expected) {
	struct CallResult query = call_resolve(label, name, 0, NULL);
	TEST_CHECK(query.result > 0 && query.result <= BUFFER_UNITS);
	union GuardedBuffer output;
	initialize_buffer(&output);
	struct CallResult exact = call_resolve(label, name, query.result, &output);
	TEST_CHECK_EQ(query.result, exact.result);
	TEST_CHECK_EQ(query.error, exact.error);
	check_text(&output, exact.result, expected);
}

static void observe_name(const char *label, LPCWSTR name) {
	union GuardedBuffer output;
	initialize_buffer(&output);
	struct CallResult result = call_resolve(label, name, BUFFER_UNITS, &output);
	if (result.result > 0)
		check_text(&output, result.result, NULL);
}

static void test_native(void) {
	test_name("user-default", NULL, NULL);
	test_name("invariant", L"", L"");
	test_name("neutral-en", L"en", L"en-US");
	test_name("specific-en", L"en-US", L"en-US");
	test_name("private-en", L"en-XA", L"en-US");
	test_name("legacy-zh", L"zh-CHS", NULL);
	test_name("simplified-zh", L"zh-Hans", NULL);
	test_name("traditional-zh", L"zh-Hant", NULL);
	union GuardedBuffer output;
	initialize_buffer(&output);
	struct CallResult present = call_resolve("present-zero", L"en-US", 0, &output);
	TEST_CHECK_EQ(6, present.result);
	initialize_buffer(&output);
	struct CallResult generous = call_resolve("generous", L"en-US", BUFFER_UNITS, &output);
	TEST_CHECK_EQ(6, generous.result);
	check_text(&output, generous.result, L"en-US");
	for (size_t index = BUFFER_PREFIX + 6 * sizeof(WCHAR); index < sizeof(output.bytes); ++index)
		TEST_CHECK_EQ(0xa5, output.bytes[index]);
	initialize_buffer(&output);
	struct CallResult short_buffer = call_resolve("short", L"en-US", 5, &output);
	TEST_CHECK_EQ(0, short_buffer.result);
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, short_buffer.error);
	printf("short-snapshot:");
	for (unsigned index = 0; index < 5 * sizeof(WCHAR); ++index)
		printf(" %02x", buffer_data(&output)[index]);
	printf("\n");
	observe_name("unknown-name", L"synthetic-unknown-locale");
	observe_name("invalid-name", L"en@US");
	initialize_buffer(&output);
	struct CallResult negative = call_resolve("negative-capacity", L"en-US", -1, &output);
	if (negative.result > 0)
		check_text(&output, negative.result, NULL);
	if (getenv("WIBO_EXPECT_RESOLVE_LIMITS")) {
		TEST_CHECK_EQ(0, negative.result);
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, negative.error);
		for (size_t index = 0; index < sizeof(output.bytes); ++index)
			TEST_CHECK_EQ(0xa5, output.bytes[index]);
	}
}

static void test_transport(const char *mode) {
	union GuardedBuffer output;
	initialize_buffer(&output);
	BOOL query = strcmp(mode, "query-data") == 0 || strcmp(mode, "query-overflow") == 0;
	int capacity = query ? 0 : strcmp(mode, "short-mutated") == 0 ? 5 : 8;
	LPCWSTR name = L"en-US";
	if (strcmp(mode, "null-name") == 0)
		name = NULL;
	else if (strcmp(mode, "invariant") == 0)
		name = L"";
	struct CallResult result = call_resolve("transport", name, capacity, query ? NULL : &output);
	int expected_result = 0;
	DWORD expected_error = ERROR_INVALID_DATA;
	BYTE expected[8 * sizeof(WCHAR)];
	memset(expected, 0xa5, sizeof(expected));
	if (strcmp(mode, "success") == 0 || strcmp(mode, "null-name") == 0 || strcmp(mode, "success-cleared") == 0) {
		expected_result = 6;
		expected_error = strcmp(mode, "success-cleared") == 0 ? ERROR_SUCCESS : 0x4321;
		memcpy(expected, L"en-US", 6 * sizeof(WCHAR));
	} else if (strcmp(mode, "invariant") == 0) {
		expected_result = 1;
		expected_error = 0x4321;
		memcpy(expected, L"", sizeof(WCHAR));
	} else if (strcmp(mode, "short-mutated") == 0) {
		expected_error = ERROR_INSUFFICIENT_BUFFER;
		memcpy(expected, L"en-U", 5 * sizeof(WCHAR));
	} else if (strcmp(mode, "false-zero") == 0)
		expected_error = ERROR_SUCCESS;
	else if (strcmp(mode, "failed") == 0)
		expected_error = ERROR_ACCESS_DENIED;
	else if (strcmp(mode, "unavailable") == 0)
		expected_error = ERROR_NOT_SUPPORTED;
	TEST_CHECK_EQ(expected_result, result.result);
	TEST_CHECK_EQ(expected_error, result.error);
	if (!query)
		TEST_CHECK(memcmp(buffer_data(&output), expected, (size_t)capacity * sizeof(WCHAR)) == 0);
	if (strcmp(mode, "success") == 0) {
		struct CallResult size = call_resolve("transport-query", name, 0, NULL);
		TEST_CHECK_EQ(6, size.result);
		TEST_CHECK_EQ(0x4321, size.error);
		initialize_buffer(&output);
		size = call_resolve("transport-present-zero", name, 0, &output);
		TEST_CHECK_EQ(6, size.result);
		TEST_CHECK_EQ(0x4321, size.error);
	}
}

static void test_local_scope(void) {
	union GuardedBuffer output;
	initialize_buffer(&output);
	struct CallResult negative = call_resolve("local-negative", L"en-US", -1, &output);
	TEST_CHECK_EQ(0, negative.result);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, negative.error);
	for (size_t index = 0; index < sizeof(output.bytes); ++index)
		TEST_CHECK_EQ(0xa5, output.bytes[index]);
	struct CallResult absent = call_resolve("local-null-positive", L"en-US", 8, NULL);
	TEST_CHECK_EQ(0, absent.result);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, absent.error);
	const int units = 20000;
	BYTE *large = malloc((size_t)units * sizeof(WCHAR));
	TEST_CHECK(large != NULL);
	memset(large, 0xa5, (size_t)units * sizeof(WCHAR));
	SetLastError(0x4321);
	int result = resolve_locale(L"en-US", (LPWSTR)large, units);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(0, result);
	TEST_CHECK_EQ(ERROR_NOT_ENOUGH_MEMORY, error);
	for (size_t index = 0; index < (size_t)units * sizeof(WCHAR); ++index)
		TEST_CHECK_EQ(0xa5, large[index]);
	free(large);
}

int main(void) {
	HMODULE module = GetModuleHandleW(L"kernel32.dll");
	TEST_CHECK(module != NULL);
	FARPROC exported = GetProcAddress(module, "ResolveLocaleName");
	_Static_assert(sizeof(exported) == sizeof(resolve_locale), "Resolved function pointer width");
	memcpy(&resolve_locale, &exported, sizeof(resolve_locale));
	TEST_CHECK(resolve_locale != NULL);
	const char *mode = getenv("WIBO_FIXTURE_RESOLVE_RESPONSE");
	if (getenv("WIBO_FIXTURE_RESOLVE_LOCAL"))
		test_local_scope();
	else if (mode)
		test_transport(mode);
	else
		test_native();
	return 0;
}
