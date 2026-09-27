#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

typedef BOOL(WINAPI *GetLanguagesFn)(DWORD, PULONG, PZZWSTR, PULONG);

struct GuardedScalar {
	BYTE before[16];
	ULONG value;
	BYTE after[16];
};

struct GuardedBuffer {
	BYTE *storage;
	WCHAR *text;
	ULONG capacity;
};

struct LanguageList {
	struct GuardedBuffer buffer;
	ULONG count;
	ULONG units;
};

struct CallResult {
	BOOL success;
	DWORD error;
};

_Static_assert(sizeof(WCHAR) == 2, "Language lists contain UTF-16 code units");
_Static_assert(sizeof(ULONG) == 4, "Language outputs are four-byte integers");

static GetLanguagesFn get_languages;

static struct GuardedScalar scalar(ULONG value) {
	struct GuardedScalar result;
	memset(&result, 0xa5, sizeof(result));
	result.value = value;
	return result;
}

static void check_scalar(const struct GuardedScalar *value) {
	for (unsigned index = 0; index < sizeof(value->before); ++index) {
		TEST_CHECK_EQ(0xa5, value->before[index]);
		TEST_CHECK_EQ(0xa5, value->after[index]);
	}
}

static struct GuardedBuffer buffer(ULONG capacity) {
	TEST_CHECK(capacity <= 65536);
	struct GuardedBuffer result;
	result.capacity = capacity;
	result.storage = malloc(32 + (size_t)capacity * sizeof(WCHAR));
	TEST_CHECK(result.storage != NULL);
	memset(result.storage, 0xa5, 32 + (size_t)capacity * sizeof(WCHAR));
	result.text = (WCHAR *)(result.storage + 16);
	return result;
}

static void check_buffer(const struct GuardedBuffer *value, ULONG written) {
	TEST_CHECK(written <= value->capacity);
	for (unsigned index = 0; index < 16; ++index) {
		TEST_CHECK_EQ(0xa5, value->storage[index]);
		TEST_CHECK_EQ(0xa5, value->storage[16 + (size_t)value->capacity * sizeof(WCHAR) + index]);
	}
	for (ULONG index = written; index < value->capacity; ++index)
		TEST_CHECK_EQ(0xa5a5, value->text[index]);
}

static struct CallResult call_languages(const char *name, DWORD flags, struct GuardedScalar *count, WCHAR *text,
										struct GuardedScalar *size) {
	SetLastError(0x4321);
	struct CallResult result;
	result.success = get_languages(flags, &count->value, text, &size->value);
	result.error = GetLastError();
	check_scalar(count);
	check_scalar(size);
	printf("%s: flags=%lu success=%d count=%lu size=%lu error=%lu\n", name, (unsigned long)flags, result.success,
		   (unsigned long)count->value, (unsigned long)size->value, (unsigned long)result.error);
	return result;
}

static void check_success(struct CallResult result) {
	TEST_CHECK(result.success != FALSE);
	TEST_CHECK_EQ(0x4321, result.error);
}

static void check_failure(struct CallResult result, DWORD error) {
	TEST_CHECK_EQ(FALSE, result.success);
	TEST_CHECK_EQ(error, result.error);
}

static void check_list(const struct LanguageList *list, DWORD flags) {
	TEST_CHECK(list->count > 0 && list->units >= 2);
	TEST_CHECK_EQ(0, list->buffer.text[list->units - 1]);
	TEST_CHECK_EQ(0, list->buffer.text[list->units - 2]);
	ULONG position = 0, entries = 0;
	while (position + 1 < list->units) {
		const ULONG start = position;
		while (position < list->units && list->buffer.text[position])
			++position;
		TEST_CHECK(position > start && position < list->units - 1);
		if (flags & MUI_LANGUAGE_ID) {
			TEST_CHECK_EQ(4, position - start);
			for (ULONG index = start; index < position; ++index) {
				const WCHAR c = list->buffer.text[index];
				TEST_CHECK((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'));
			}
		} else {
			TEST_CHECK(position - start < LOCALE_NAME_MAX_LENGTH);
		}
		for (ULONG earlier = 0; earlier < start;) {
			ULONG end = earlier;
			while (list->buffer.text[end])
				++end;
			TEST_CHECK(end - earlier != position - start ||
					   memcmp(list->buffer.text + earlier, list->buffer.text + start,
							  (size_t)(position - start) * sizeof(WCHAR)) != 0);
			earlier = end + 1;
		}
		++entries;
		++position;
	}
	TEST_CHECK_EQ(list->units - 1, position);
	TEST_CHECK_EQ(list->count, entries);
	check_buffer(&list->buffer, list->units);
}

static struct LanguageList collect_languages(DWORD flags) {
	struct GuardedScalar count = scalar(0x11223344), size = scalar(0);
	check_success(call_languages("query", flags, &count, NULL, &size));
	TEST_CHECK(count.value > 0 && size.value >= 2 && size.value <= 65531);
	struct LanguageList list = {buffer(size.value + 5), count.value, size.value};
	count = scalar(0x11223344);
	size = scalar(list.buffer.capacity);
	check_success(call_languages("fill", flags, &count, list.buffer.text, &size));
	TEST_CHECK_EQ(list.count, count.value);
	TEST_CHECK_EQ(list.units, size.value);
	check_list(&list, flags);

	struct GuardedBuffer repeated = buffer(list.units);
	count = scalar(0x11223344);
	size = scalar(list.units);
	check_success(call_languages("exact-repeat", flags, &count, repeated.text, &size));
	TEST_CHECK_EQ(list.count, count.value);
	TEST_CHECK_EQ(list.units, size.value);
	TEST_CHECK(memcmp(list.buffer.text, repeated.text, (size_t)list.units * sizeof(WCHAR)) == 0);
	check_buffer(&repeated, list.units);
	free(repeated.storage);

	struct GuardedBuffer short_buffer = buffer(list.units);
	count = scalar(0x11223344);
	size = scalar(list.units - 1);
	check_failure(call_languages("short-buffer", flags, &count, short_buffer.text, &size), ERROR_INSUFFICIENT_BUFFER);
	TEST_CHECK_EQ(list.units, size.value);
	check_buffer(&short_buffer, list.units);
	printf("short-buffer-unchanged=%d\n", short_buffer.text[0] == 0xa5a5);
	free(short_buffer.storage);
	return list;
}

static void test_languages(void) {
	struct LanguageList default_list = collect_languages(0);
	struct LanguageList names = collect_languages(MUI_LANGUAGE_NAME);
	struct LanguageList identifiers = collect_languages(MUI_LANGUAGE_ID);
	TEST_CHECK_EQ(default_list.count, names.count);
	TEST_CHECK_EQ(default_list.units, names.units);
	TEST_CHECK(memcmp(default_list.buffer.text, names.buffer.text, (size_t)names.units * sizeof(WCHAR)) == 0);

	struct GuardedBuffer output = buffer(names.units);
	struct GuardedScalar count = scalar(0x11223344), size = scalar(0);
	check_failure(call_languages("present-zero-capacity", MUI_LANGUAGE_NAME, &count, output.text, &size),
				  ERROR_INSUFFICIENT_BUFFER);
	TEST_CHECK_EQ(names.units, size.value);
	check_buffer(&output, 0);
	free(output.storage);
	free(default_list.buffer.storage);
	free(names.buffer.storage);
	free(identifiers.buffer.storage);
}

static void test_errors(void) {
	const DWORD flags[] = {MUI_LANGUAGE_NAME | MUI_LANGUAGE_ID, 0x80000000};
	for (unsigned index = 0; index < sizeof(flags) / sizeof(flags[0]); ++index) {
		struct GuardedScalar count = scalar(0x11223344), size = scalar(0);
		check_failure(call_languages("invalid-flags", flags[index], &count, NULL, &size), ERROR_INVALID_PARAMETER);
	}
}

static void test_local_scope(void) {
	test_errors();
	struct GuardedScalar count = scalar(0x11223344), size = scalar(0);
	SetLastError(0x4321);
	TEST_CHECK_EQ(FALSE, get_languages(MUI_LANGUAGE_NAME, NULL, NULL, &size.value));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK_EQ(0, size.value);
	check_scalar(&size);
	SetLastError(0x4321);
	TEST_CHECK_EQ(FALSE, get_languages(MUI_LANGUAGE_NAME, &count.value, NULL, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK_EQ(0x11223344, count.value);
	check_scalar(&count);
}

static void test_transport(const char *mode) {
	if (strcmp(mode, "success") == 0) {
		test_languages();
		test_errors();
		return;
	}
	const BOOL short_size = strcmp(mode, "short-size") == 0;
	const BOOL short_count = strcmp(mode, "short-count") == 0;
	const BOOL null_short = strcmp(mode, "null-positive-short") == 0;
	const BOOL huge_query = strcmp(mode, "query-huge-size") == 0;
	const BOOL null_buffer = null_short || huge_query || strcmp(mode, "null-positive-success") == 0;
	const BOOL zero_capacity = strcmp(mode, "present-zero-success") == 0;
	const BOOL identifiers = strcmp(mode, "invalid-identifier") == 0;
	struct GuardedBuffer output = buffer(32);
	ULONG initial_size = 32;
	if (zero_capacity || huge_query)
		initial_size = 0;
	else if (short_size || short_count)
		initial_size = 1;
	struct GuardedScalar count = scalar(0x11223344), size = scalar(initial_size);
	struct CallResult result = call_languages("transport", identifiers ? MUI_LANGUAGE_ID : MUI_LANGUAGE_NAME, &count,
											  null_buffer ? NULL : output.text, &size);
	DWORD expected_error = ERROR_INVALID_DATA;
	ULONG expected_size = initial_size;
	if (short_size || short_count || null_short) {
		expected_error = ERROR_INSUFFICIENT_BUFFER;
		expected_size = 13;
	} else if (strcmp(mode, "failed") == 0)
		expected_error = ERROR_INVALID_PARAMETER;
	else if (strcmp(mode, "failed-zero") == 0)
		expected_error = ERROR_SUCCESS;
	else if (strcmp(mode, "unavailable") == 0)
		expected_error = ERROR_NOT_SUPPORTED;
	check_failure(result, expected_error);
	TEST_CHECK_EQ(short_count ? 2 : 0x11223344, count.value);
	TEST_CHECK_EQ(expected_size, size.value);
	check_buffer(&output, 0);
	free(output.storage);
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "GetUserPreferredUILanguages");
	_Static_assert(sizeof(exported) == sizeof(get_languages), "Resolved function pointers have the same width");
	memcpy(&get_languages, &exported, sizeof(get_languages));
	TEST_CHECK(get_languages != NULL);
	const char *mode = getenv("WIBO_FIXTURE_UI_LANGUAGES_RESPONSE");
	if (mode)
		test_transport(mode);
	else if (getenv("WIBO_EXPECT_UI_LANGUAGES_LOCAL"))
		test_local_scope();
	else {
		test_languages();
		test_errors();
	}
	return 0;
}
