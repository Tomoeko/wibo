#include "test_assert.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>

#include <wchar.h>

static void list_path(WCHAR *output, size_t capacity, const WCHAR *path) {
	size_t length = wcslen(path);
	TEST_CHECK(length + 2 <= capacity);
	memcpy(output, path, (length + 1) * sizeof(WCHAR));
	output[length + 1] = L'\0';
}

static void write_byte(const WCHAR *path, char value) {
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	DWORD written = 0;
	TEST_CHECK(WriteFile(file, &value, 1, &written, NULL));
	TEST_CHECK_EQ(1, written);
	TEST_CHECK(CloseHandle(file));
}

static void check_byte(const WCHAR *path, char expected) {
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	char value = 0;
	DWORD read = 0;
	TEST_CHECK(ReadFile(file, &value, 1, &read, NULL));
	TEST_CHECK_EQ(1, read);
	TEST_CHECK_EQ(expected, value);
	TEST_CHECK(CloseHandle(file));
}

static void operate(UINT action, const WCHAR *from, const WCHAR *to) {
	WCHAR fromList[MAX_PATH + 2], toList[MAX_PATH + 2];
	list_path(fromList, MAX_PATH + 2, from);
	if (to)
		list_path(toList, MAX_PATH + 2, to);
	SHFILEOPSTRUCTW operation = {0};
	operation.wFunc = action;
	operation.pFrom = fromList;
	operation.pTo = to ? toList : NULL;
	operation.fFlags = FOF_NO_UI;
	TEST_CHECK_EQ(0, SHFileOperationW(&operation));
	TEST_CHECK(!operation.fAnyOperationsAborted);
}

int main(void) {
	WCHAR temporary[MAX_PATH], directory[MAX_PATH], source[MAX_PATH], copied[MAX_PATH];
	WCHAR moved[MAX_PATH], renamed[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH, temporary);
	TEST_CHECK(length > 0 && length < MAX_PATH);
	TEST_CHECK(
		swprintf(directory, MAX_PATH, L"%lsshell_file_%lu_%lu", temporary, GetCurrentProcessId(), GetTickCount()) > 0);
	TEST_CHECK(CreateDirectoryW(directory, NULL));
	TEST_CHECK(swprintf(source, MAX_PATH, L"%ls\\caf\u00e9.txt", directory) > 0);
	TEST_CHECK(swprintf(copied, MAX_PATH, L"%ls\\copy.txt", directory) > 0);
	TEST_CHECK(swprintf(moved, MAX_PATH, L"%ls\\moved.txt", directory) > 0);
	TEST_CHECK(swprintf(renamed, MAX_PATH, L"%ls\\renamed.txt", directory) > 0);
	write_byte(source, 'A');
	operate(FO_COPY, source, copied);
	check_byte(source, 'A');
	check_byte(copied, 'A');
	operate(FO_MOVE, copied, moved);
	TEST_CHECK(GetFileAttributesW(copied) == INVALID_FILE_ATTRIBUTES);
	check_byte(moved, 'A');
	operate(FO_RENAME, moved, renamed);
	TEST_CHECK(GetFileAttributesW(moved) == INVALID_FILE_ATTRIBUTES);
	check_byte(renamed, 'A');
	operate(FO_DELETE, renamed, NULL);
	TEST_CHECK(GetFileAttributesW(renamed) == INVALID_FILE_ATTRIBUTES);
	TEST_CHECK(DeleteFileW(source));
	TEST_CHECK(RemoveDirectoryW(directory));
	return 0;
}
