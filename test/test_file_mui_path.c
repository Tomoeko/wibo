#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

#include <wchar.h>

typedef BOOL(WINAPI *FileMuiPathFn)(DWORD, PCWSTR, PWSTR, PULONG, PWSTR, PULONG, PULONGLONG);

struct GuardedText {
	WCHAR before[8];
	WCHAR text[MAX_PATH];
	WCHAR after[8];
};

struct GuardedSize {
	ULONG before;
	ULONG value;
	ULONG after;
};

struct GuardedEnumerator {
	ULONGLONG before;
	ULONGLONG value;
	ULONGLONG after;
};

_Static_assert(sizeof(WCHAR) == 2, "MUI buffers count UTF16 units");
_Static_assert(sizeof(ULONG) == 4, "MUI buffer size width");
_Static_assert(sizeof(ULONGLONG) == 8, "MUI enumeration state width");

static FileMuiPathFn get_file_mui_path;
static DWORD expected_error = MAXDWORD;

static void path_join(LPCWSTR directory, LPCWSTR leaf, WCHAR output[MAX_PATH]) {
	TEST_CHECK(wcslen(directory) + 1 + wcslen(leaf) < MAX_PATH);
	wcscpy(output, directory);
	wcscat(output, L"\\");
	wcscat(output, leaf);
}

static void write_file(LPCWSTR path) {
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	const char content[] = "Synthetic localized resource\n";
	DWORD written;
	TEST_CHECK(WriteFile(file, content, sizeof(content) - 1, &written, NULL));
	TEST_CHECK_EQ(sizeof(content) - 1, written);
	TEST_CHECK(CloseHandle(file));
}

static void check_text_bounds(const struct GuardedText *before, const struct GuardedText *after, ULONG capacity) {
	TEST_CHECK(capacity <= MAX_PATH);
	TEST_CHECK(memcmp(before->before, after->before, sizeof(before->before)) == 0);
	TEST_CHECK(memcmp(before->text + capacity, after->text + capacity, (MAX_PATH - capacity) * sizeof(WCHAR)) == 0);
	TEST_CHECK(memcmp(before->after, after->after, sizeof(before->after)) == 0);
}

static void observe(const char *label, DWORD flags, LPCWSTR file, LPCWSTR language, ULONG language_capacity,
					ULONG path_capacity, BOOL language_present, BOOL path_present) {
	struct GuardedText language_buffer, path_buffer;
	memset(&language_buffer, 0xa5, sizeof(language_buffer));
	memset(&path_buffer, 0xa5, sizeof(path_buffer));
	if (language_present) {
		TEST_CHECK(language != NULL && wcslen(language) < MAX_PATH);
		wcscpy(language_buffer.text, language);
	}
	struct GuardedText language_before = language_buffer, path_before = path_buffer;
	struct GuardedSize language_size, path_size;
	struct GuardedEnumerator enumerator;
	memset(&language_size, 0xa5, sizeof(language_size));
	memset(&path_size, 0xa5, sizeof(path_size));
	memset(&enumerator, 0xa5, sizeof(enumerator));
	language_size.value = language_capacity;
	path_size.value = path_capacity;
	enumerator.value = 0;
	const struct GuardedSize language_size_before = language_size, path_size_before = path_size;
	const struct GuardedEnumerator enumerator_before = enumerator;
	SetLastError(0x4321);
	BOOL result = get_file_mui_path(flags, file, language_present ? language_buffer.text : NULL, &language_size.value,
									path_present ? path_buffer.text : NULL, &path_size.value, &enumerator.value);
	DWORD error = GetLastError();
	check_text_bounds(&language_before, &language_buffer, language_capacity);
	check_text_bounds(&path_before, &path_buffer, path_capacity);
	TEST_CHECK_EQ(language_size_before.before, language_size.before);
	TEST_CHECK_EQ(language_size_before.after, language_size.after);
	TEST_CHECK_EQ(path_size_before.before, path_size.before);
	TEST_CHECK_EQ(path_size_before.after, path_size.after);
	TEST_CHECK_U64_EQ(enumerator_before.before, enumerator.before);
	TEST_CHECK_U64_EQ(enumerator_before.after, enumerator.after);
	BOOL language_changed = memcmp(&language_before, &language_buffer, sizeof(language_buffer)) != 0;
	BOOL path_changed = memcmp(&path_before, &path_buffer, sizeof(path_buffer)) != 0;
	printf("%s: flags=%lx result=%ld error=%lu sizes=%lu,%lu->%lu,%lu enumerator=%llu changed=%ld,%ld\n", label,
		   (unsigned long)flags, (long)result, (unsigned long)error, (unsigned long)language_capacity,
		   (unsigned long)path_capacity, (unsigned long)language_size.value, (unsigned long)path_size.value,
		   (unsigned long long)enumerator.value, (long)language_changed, (long)path_changed);
	if (error == ERROR_CALL_NOT_IMPLEMENTED) {
		TEST_CHECK_EQ(FALSE, result);
	}
	if (error == ERROR_CALL_NOT_IMPLEMENTED || expected_error != MAXDWORD) {
		if (expected_error != MAXDWORD) {
			TEST_CHECK_EQ(FALSE, result);
			TEST_CHECK_EQ(expected_error, error);
		}
		TEST_CHECK(!language_changed && !path_changed);
		TEST_CHECK(memcmp(&language_size_before, &language_size, sizeof(language_size)) == 0);
		TEST_CHECK(memcmp(&path_size_before, &path_size, sizeof(path_size)) == 0);
		TEST_CHECK(memcmp(&enumerator_before, &enumerator, sizeof(enumerator)) == 0);
	}
}

static void test_native(void) {
	WCHAR temporary[MAX_PATH], root[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH, temporary);
	TEST_CHECK(length > 0 && length < MAX_PATH);
	TEST_CHECK(GetTempFileNameW(temporary, L"MUI", 0, root));
	TEST_CHECK(DeleteFileW(root));
	TEST_CHECK(CreateDirectoryW(root, NULL));
	WCHAR source[MAX_PATH], en_directory[MAX_PATH], ja_directory[MAX_PATH];
	WCHAR en_file[MAX_PATH], ja_file[MAX_PATH], missing[MAX_PATH];
	path_join(root, L"fixture.txt", source);
	path_join(root, L"en-US", en_directory);
	path_join(root, L"ja-JP", ja_directory);
	path_join(en_directory, L"fixture.txt", en_file);
	path_join(ja_directory, L"fixture.txt", ja_file);
	path_join(root, L"missing.txt", missing);
	TEST_CHECK(CreateDirectoryW(en_directory, NULL));
	TEST_CHECK(CreateDirectoryW(ja_directory, NULL));
	write_file(source);
	write_file(en_file);
	write_file(ja_file);
	const DWORD search = MUI_LANGUAGE_NAME | MUI_USE_SEARCH_ALL_LANGUAGES | MUI_NON_LANG_NEUTRAL_FILE;
	BOOL expect_limits = getenv("WIBO_EXPECT_FILE_MUI_LIMITS") != NULL;
	expected_error = expect_limits ? ERROR_CALL_NOT_IMPLEMENTED : MAXDWORD;
	observe("query", search, source, NULL, 0, 0, FALSE, FALSE);
	if (expect_limits)
		expected_error = ERROR_NOT_SUPPORTED;
	observe("present-zero", search, source, L"", 0, 0, TRUE, TRUE);
	expected_error = expect_limits ? ERROR_CALL_NOT_IMPLEMENTED : MAXDWORD;
	observe("generous", search, source, L"", LOCALE_NAME_MAX_LENGTH, MAX_PATH, TRUE, TRUE);
	observe("language-query", search, source, NULL, 0, MAX_PATH, FALSE, TRUE);
	observe("path-query", search, source, L"", LOCALE_NAME_MAX_LENGTH, 0, TRUE, FALSE);
	observe("short", search, source, L"", 1, 1, TRUE, TRUE);
	observe("explicit-name", MUI_LANGUAGE_NAME | MUI_NON_LANG_NEUTRAL_FILE, source, L"en-US", LOCALE_NAME_MAX_LENGTH,
			MAX_PATH, TRUE, TRUE);
	observe("explicit-id", MUI_LANGUAGE_ID | MUI_NON_LANG_NEUTRAL_FILE, source, L"0409", LOCALE_NAME_MAX_LENGTH,
			MAX_PATH, TRUE, TRUE);
	observe("missing", search, missing, L"", LOCALE_NAME_MAX_LENGTH, MAX_PATH, TRUE, TRUE);
	TEST_CHECK(DeleteFileW(en_file));
	TEST_CHECK(DeleteFileW(ja_file));
	TEST_CHECK(DeleteFileW(source));
	TEST_CHECK(RemoveDirectoryW(en_directory));
	TEST_CHECK(RemoveDirectoryW(ja_directory));
	TEST_CHECK(RemoveDirectoryW(root));
}

static void test_transport(const char *mode) {
	expected_error = ERROR_INVALID_DATA;
	if (strcmp(mode, "unchanged") == 0 || strcmp(mode, "query") == 0 || strcmp(mode, "long-filename") == 0)
		expected_error = ERROR_CALL_NOT_IMPLEMENTED;
	else if (strcmp(mode, "changed-size") == 0 || strcmp(mode, "changed-language") == 0 ||
			 strcmp(mode, "changed-path") == 0 || strcmp(mode, "changed-enumerator") == 0 ||
			 strcmp(mode, "true") == 0 || strcmp(mode, "other-error") == 0 || strcmp(mode, "unavailable") == 0)
		expected_error = ERROR_NOT_SUPPORTED;
	else if (strcmp(mode, "failed") == 0)
		expected_error = ERROR_ACCESS_DENIED;
	const DWORD flags = MUI_LANGUAGE_NAME | MUI_USE_SEARCH_ALL_LANGUAGES | MUI_NON_LANG_NEUTRAL_FILE;
	BOOL query = strcmp(mode, "query") == 0;
	WCHAR long_filename[129];
	for (size_t index = 0; index < 128; ++index)
		long_filename[index] = L'a';
	long_filename[128] = 0;
	LPCWSTR filename = strcmp(mode, "long-filename") == 0 ? long_filename : L"C:\\Fixture\\resource.dll";
	observe("transport", flags, filename, query ? NULL : L"en-US", query ? 0 : 8, query ? 0 : 16, !query, !query);
}

static void test_local_scope(void) {
	const DWORD flags = MUI_LANGUAGE_NAME | MUI_USE_SEARCH_ALL_LANGUAGES | MUI_NON_LANG_NEUTRAL_FILE;
	expected_error = ERROR_NOT_SUPPORTED;
	observe("local-present-zero", flags, L"fixture.dll", L"", 0, 0, TRUE, TRUE);
	observe("local-null-positive-language", flags, L"fixture.dll", NULL, 8, 16, FALSE, TRUE);
	observe("local-null-positive-path", flags, L"fixture.dll", L"en-US", 8, 16, TRUE, FALSE);
	observe("local-null-filename", flags, NULL, L"en-US", 8, 16, TRUE, TRUE);
	observe("local-unterminated-language", flags, L"fixture.dll", L"en-US", 3, 16, TRUE, TRUE);
	struct GuardedText language, path;
	memset(&language, 0xa5, sizeof(language));
	memset(&path, 0xa5, sizeof(path));
	wcscpy(language.text, L"en-US");
	const struct GuardedText language_before = language, path_before = path;
	struct GuardedSize language_size, path_size;
	struct GuardedEnumerator enumerator;
	memset(&language_size, 0xa5, sizeof(language_size));
	memset(&path_size, 0xa5, sizeof(path_size));
	memset(&enumerator, 0xa5, sizeof(enumerator));
	language_size.value = 8;
	path_size.value = 16;
	enumerator.value = 0;
	const struct GuardedSize language_size_before = language_size, path_size_before = path_size;
	const struct GuardedEnumerator enumerator_before = enumerator;
	for (unsigned missing = 0; missing < 3; ++missing) {
		SetLastError(0x4321);
		BOOL result = get_file_mui_path(
			flags, L"fixture.dll", language.text, missing == 0 ? NULL : &language_size.value, path.text,
			missing == 1 ? NULL : &path_size.value, missing == 2 ? NULL : &enumerator.value);
		TEST_CHECK_EQ(FALSE, result);
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK(memcmp(&language_before, &language, sizeof(language)) == 0);
		TEST_CHECK(memcmp(&path_before, &path, sizeof(path)) == 0);
		TEST_CHECK(memcmp(&language_size_before, &language_size, sizeof(language_size)) == 0);
		TEST_CHECK(memcmp(&path_size_before, &path_size, sizeof(path_size)) == 0);
		TEST_CHECK(memcmp(&enumerator_before, &enumerator, sizeof(enumerator)) == 0);
	}
	const size_t large_units = 20000;
	WCHAR *filename = malloc((large_units + 1) * sizeof(WCHAR));
	TEST_CHECK(filename != NULL);
	for (size_t index = 0; index < large_units; ++index)
		filename[index] = L'a';
	filename[large_units] = 0;
	expected_error = ERROR_NOT_ENOUGH_MEMORY;
	observe("local-large-filename", flags, filename, L"en-US", 8, 16, TRUE, TRUE);
	for (size_t index = 0; index < large_units; ++index)
		TEST_CHECK_EQ(L'a', filename[index]);
	free(filename);
	WCHAR *large_language = malloc(large_units * sizeof(WCHAR));
	TEST_CHECK(large_language != NULL);
	memset(large_language, 0xa5, large_units * sizeof(WCHAR));
	large_language[0] = 0;
	language_size.value = (ULONG)large_units;
	const struct GuardedSize large_size_before = language_size;
	SetLastError(0x4321);
	BOOL result = get_file_mui_path(flags, L"fixture.dll", large_language, &language_size.value, path.text,
									&path_size.value, &enumerator.value);
	TEST_CHECK_EQ(FALSE, result);
	TEST_CHECK_EQ(ERROR_NOT_ENOUGH_MEMORY, GetLastError());
	TEST_CHECK_EQ(0, large_language[0]);
	for (size_t index = 1; index < large_units; ++index)
		TEST_CHECK_EQ(0xa5a5, large_language[index]);
	TEST_CHECK(memcmp(&large_size_before, &language_size, sizeof(language_size)) == 0);
	TEST_CHECK(memcmp(&path_size_before, &path_size, sizeof(path_size)) == 0);
	TEST_CHECK(memcmp(&enumerator_before, &enumerator, sizeof(enumerator)) == 0);
	TEST_CHECK(memcmp(&path_before, &path, sizeof(path)) == 0);
	free(large_language);
}

int main(void) {
	HMODULE module = GetModuleHandleW(L"kernel32.dll");
	TEST_CHECK(module != NULL);
	FARPROC exported = GetProcAddress(module, "GetFileMUIPath");
	_Static_assert(sizeof(exported) == sizeof(get_file_mui_path), "Resolved MUI function pointer width");
	memcpy(&get_file_mui_path, &exported, sizeof(get_file_mui_path));
	TEST_CHECK(get_file_mui_path != NULL);
	const char *mode = getenv("WIBO_FIXTURE_FILE_MUI_RESPONSE");
	if (getenv("WIBO_FIXTURE_FILE_MUI_LOCAL"))
		test_local_scope();
	else if (mode)
		test_transport(mode);
	else
		test_native();
	return 0;
}
