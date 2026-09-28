#include "test_assert.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shlwapi.h>

#include <wchar.h>

int main(void) {
	const WCHAR absolute[] = L"C:\\folder\\file.txt";
	const WCHAR relative[] = L"folder\\file.txt";
	TEST_CHECK(!PathIsRelativeW(absolute));
	TEST_CHECK(!PathIsRelativeW(L"\\folder\\file.txt"));
	TEST_CHECK(PathIsRelativeW(relative));
	TEST_CHECK(wcscmp(PathFindFileNameW(absolute), L"file.txt") == 0);
	TEST_CHECK(PathFindFileNameW(relative) == relative + 7);

	WCHAR temporary[MAX_PATH], directory[MAX_PATH], fileName[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH, temporary);
	TEST_CHECK(length > 0 && length < MAX_PATH);
	TEST_CHECK(
		swprintf(directory, MAX_PATH, L"%lspath_empty_%lu_%lu", temporary, GetCurrentProcessId(), GetTickCount()) > 0);
	TEST_CHECK(CreateDirectoryW(directory, NULL));
	TEST_CHECK(PathIsDirectoryEmptyW(directory));
	TEST_CHECK(swprintf(fileName, MAX_PATH, L"%ls\\entry.txt", directory) > 0);
	HANDLE file = CreateFileW(fileName, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(!PathIsDirectoryEmptyW(directory));
	TEST_CHECK(!PathIsDirectoryEmptyW(fileName));
	TEST_CHECK(DeleteFileW(fileName));
	TEST_CHECK(RemoveDirectoryW(directory));
	return 0;
}
