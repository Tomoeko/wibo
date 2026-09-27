#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

#include <stdint.h>
#include <wchar.h>

typedef int(WINAPI *LocaleInfoFn)(LPCWSTR, LCTYPE, LPWSTR, int);

enum { BUFFER_UNITS = 64, BUFFER_PREFIX = 16 };

union GuardedBuffer {
	DWORD alignment;
	BYTE bytes[BUFFER_PREFIX + 1 + BUFFER_UNITS * sizeof(WCHAR) + 16];
};

struct CallResult {
	int result;
	DWORD error;
};

_Static_assert(sizeof(WCHAR) == 2, "Locale buffer capacities count UTF16 units");
_Static_assert(sizeof(LOCALESIGNATURE) == 32, "Locale font signatures contain 32 raw bytes");

static LocaleInfoFn get_locale_info;

static void initialize_buffer(union GuardedBuffer *buffer) { memset(buffer, 0xa5, sizeof(*buffer)); }

static BYTE *buffer_data(union GuardedBuffer *buffer, BOOL odd) { return buffer->bytes + BUFFER_PREFIX + odd; }

static void check_bounds(const union GuardedBuffer *buffer, int capacity, BOOL odd) {
	const size_t begin = BUFFER_PREFIX + odd;
	const size_t length = capacity > 0 ? (size_t)capacity * sizeof(WCHAR) : 0;
	TEST_CHECK(capacity <= BUFFER_UNITS);
	for (size_t index = 0; index < sizeof(buffer->bytes); ++index)
		if (index < begin || index >= begin + length)
			TEST_CHECK_EQ(0xa5, buffer->bytes[index]);
}

static struct CallResult call_info(const char *label, LPCWSTR locale, LCTYPE type, int capacity,
								   union GuardedBuffer *output, BOOL odd) {
	SetLastError(0x4321);
	struct CallResult result;
	result.result = get_locale_info(locale, type, output ? (LPWSTR)buffer_data(output, odd) : NULL, capacity);
	result.error = GetLastError();
	if (output)
		check_bounds(output, capacity, odd);
	printf("%s: type=%lu capacity=%d result=%d error=%lu odd=%d\n", label, (unsigned long)type, capacity, result.result,
		   (unsigned long)result.error, odd);
	return result;
}

static void test_text(LPCWSTR locale, LCTYPE type, LPCWSTR expected) {
	struct CallResult query = call_info("text-query", locale, type, 0, NULL, FALSE);
	TEST_CHECK(query.result > 0 && query.result <= BUFFER_UNITS);
	union GuardedBuffer output;
	initialize_buffer(&output);
	struct CallResult present = call_info("present-query", locale, type, 0, &output, FALSE);
	TEST_CHECK_EQ(query.result, present.result);
	TEST_CHECK_EQ(query.error, present.error);
	struct CallResult exact = call_info("text-exact", locale, type, query.result, &output, FALSE);
	TEST_CHECK_EQ(query.result, exact.result);
	WCHAR text[BUFFER_UNITS];
	memcpy(text, buffer_data(&output, FALSE), (size_t)exact.result * sizeof(WCHAR));
	TEST_CHECK_EQ(0, text[exact.result - 1]);
	TEST_CHECK_EQ(exact.result - 1, wcslen(text));
	if (expected)
		TEST_CHECK_EQ(0, wcscmp(expected, text));
	if (query.result > 1) {
		initialize_buffer(&output);
		struct CallResult short_buffer = call_info("text-short", locale, type, query.result - 1, &output, FALSE);
		TEST_CHECK_EQ(0, short_buffer.result);
		TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, short_buffer.error);
	}
}

static void test_number(LPCWSTR locale, DWORD expected) {
	const LCTYPE type = LOCALE_INEUTRAL | LOCALE_RETURN_NUMBER;
	struct CallResult query = call_info("number-query", locale, type, 0, NULL, FALSE);
	TEST_CHECK_EQ(2, query.result);
	union GuardedBuffer output;
	initialize_buffer(&output);
	TEST_CHECK(((uintptr_t)buffer_data(&output, TRUE) & 1) != 0);
	struct CallResult exact = call_info("number-odd", locale, type, 2, &output, TRUE);
	TEST_CHECK_EQ(2, exact.result);
	DWORD number;
	memcpy(&number, buffer_data(&output, TRUE), sizeof(number));
	TEST_CHECK_EQ(expected, number);
	initialize_buffer(&output);
	struct CallResult short_buffer = call_info("number-short", locale, type, 1, &output, TRUE);
	TEST_CHECK_EQ(0, short_buffer.result);
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, short_buffer.error);
}

static void test_native(void) {
	test_text(LOCALE_NAME_USER_DEFAULT, LOCALE_SNAME, NULL);
	test_text(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_SNAME, NULL);
	test_text(L"en-US", LOCALE_SNAME, L"en-US");
	test_text(L"en-US", LOCALE_SPARENT, L"en");
	test_text(LOCALE_NAME_INVARIANT, LOCALE_SNAME, L"");
	test_number(L"en-US", 0);
	test_number(L"en", 1);
	const int signature_units = sizeof(LOCALESIGNATURE) / sizeof(WCHAR);
	struct CallResult signature_query = call_info("signature-query", L"en-US", LOCALE_FONTSIGNATURE, 0, NULL, FALSE);
	TEST_CHECK_EQ(signature_units, signature_query.result);
	union GuardedBuffer output;
	initialize_buffer(&output);
	struct CallResult signature =
		call_info("signature", L"en-US", LOCALE_FONTSIGNATURE, signature_units, &output, FALSE);
	TEST_CHECK_EQ(signature_units, signature.result);
	initialize_buffer(&output);
	TEST_CHECK_EQ(
		0, call_info("invalid-locale", L"synthetic-invalid-locale", LOCALE_SNAME, BUFFER_UNITS, &output, FALSE).result);
	initialize_buffer(&output);
	TEST_CHECK_EQ(0, call_info("invalid-type", L"en-US", 0xffff, BUFFER_UNITS, &output, FALSE).result);
	initialize_buffer(&output);
	TEST_CHECK_EQ(
		0, call_info("number-on-text", L"en-US", LOCALE_SNAME | LOCALE_RETURN_NUMBER, BUFFER_UNITS, &output, FALSE)
			   .result);
	initialize_buffer(&output);
	TEST_CHECK_EQ(0, call_info("negative-capacity", L"en-US", LOCALE_SNAME, -1, &output, FALSE).result);
}

static void test_transport(const char *mode) {
	union GuardedBuffer output;
	initialize_buffer(&output);
	LPCWSTR locale = strcmp(mode, "null-locale") == 0 ? NULL : L"en-US";
	int capacity = 2;
	BOOL query = strcmp(mode, "query-data") == 0;
	if (query)
		capacity = 0;
	else if (strcmp(mode, "negative-success") == 0)
		capacity = -1;
	struct CallResult result =
		call_info("transport", locale, LOCALE_INEUTRAL | LOCALE_RETURN_NUMBER, capacity, query ? NULL : &output, TRUE);
	int expected_result = 0;
	DWORD expected_error = ERROR_INVALID_DATA;
	DWORD expected_number = 0xa5a5a5a5;
	if (strcmp(mode, "success") == 0 || strcmp(mode, "null-locale") == 0 || strcmp(mode, "success-cleared") == 0) {
		expected_result = 2;
		expected_error = strcmp(mode, "success-cleared") == 0 ? ERROR_SUCCESS : 0x4321;
		expected_number = 0x44332211;
	} else if (strcmp(mode, "false-zero") == 0)
		expected_error = ERROR_SUCCESS;
	else if (strcmp(mode, "false-mutated") == 0) {
		expected_error = ERROR_INSUFFICIENT_BUFFER;
		expected_number = 0x78563412;
	} else if (strcmp(mode, "failed") == 0)
		expected_error = ERROR_ACCESS_DENIED;
	else if (strcmp(mode, "unavailable") == 0)
		expected_error = ERROR_NOT_SUPPORTED;
	TEST_CHECK_EQ(expected_result, result.result);
	TEST_CHECK_EQ(expected_error, result.error);
	DWORD number;
	memcpy(&number, buffer_data(&output, TRUE), sizeof(number));
	TEST_CHECK_EQ(expected_number, number);
	check_bounds(&output, 2, TRUE);
	if (strcmp(mode, "success") == 0) {
		result = call_info("query-transport", locale, LOCALE_INEUTRAL | LOCALE_RETURN_NUMBER, 0, NULL, FALSE);
		TEST_CHECK_EQ(2, result.result);
		TEST_CHECK_EQ(0x4321, result.error);
		initialize_buffer(&output);
		result = call_info("present-query-transport", locale, LOCALE_INEUTRAL | LOCALE_RETURN_NUMBER, 0, &output, TRUE);
		TEST_CHECK_EQ(2, result.result);
		TEST_CHECK_EQ(0x4321, result.error);
	}
}

static void test_local_scope(void) {
	struct CallResult null_output = call_info("unsupported-null-output", L"en-US", LOCALE_SNAME, 1, NULL, FALSE);
	TEST_CHECK_EQ(0, null_output.result);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, null_output.error);
	const int large_capacity = 16 * 1024;
	const size_t allocation = (size_t)large_capacity * sizeof(WCHAR) + 32;
	BYTE *storage = malloc(allocation);
	TEST_CHECK(storage != NULL);
	memset(storage, 0xa5, allocation);
	SetLastError(0x4321);
	TEST_CHECK_EQ(0, get_locale_info(L"en-US", LOCALE_SNAME, (LPWSTR)(storage + 16), large_capacity));
	TEST_CHECK_EQ(ERROR_NOT_ENOUGH_MEMORY, GetLastError());
	for (size_t index = 0; index < allocation; ++index)
		TEST_CHECK_EQ(0xa5, storage[index]);
	free(storage);
}

int main(void) {
	HMODULE module = GetModuleHandleW(L"kernel32.dll");
	TEST_CHECK(module != NULL);
	FARPROC exported = GetProcAddress(module, "GetLocaleInfoEx");
	_Static_assert(sizeof(exported) == sizeof(get_locale_info), "Resolved function pointers have the same width");
	memcpy(&get_locale_info, &exported, sizeof(get_locale_info));
	TEST_CHECK(get_locale_info != NULL);
	const char *mode = getenv("WIBO_FIXTURE_LOCALE_INFO_RESPONSE");
	if (mode)
		test_transport(mode);
	else if (getenv("WIBO_EXPECT_LOCALE_INFO_LOCAL"))
		test_local_scope();
	else
		test_native();
	return 0;
}
