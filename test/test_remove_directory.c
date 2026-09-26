#include "test_assert.h"
#include <wchar.h>
#include <windows.h>

int main(void) {
	const char *directory = "wibo_remove_directory_fixture";
	TEST_CHECK(CreateDirectoryA(directory, NULL));
	SetLastError(0x1234);
	TEST_CHECK(RemoveDirectoryW(L"wibo_remove_directory_fixture"));
	TEST_CHECK_EQ(0x1234, GetLastError());
	TEST_CHECK(!RemoveDirectoryW(L"wibo_remove_directory_fixture"));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	TEST_CHECK(CreateDirectoryA(directory, NULL));
	HANDLE file = CreateFileA("wibo_remove_directory_fixture/entry.bin", GENERIC_WRITE, 7, NULL, CREATE_NEW,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(!RemoveDirectoryW(L"wibo_remove_directory_fixture"));
	TEST_CHECK_EQ(ERROR_DIR_NOT_EMPTY, GetLastError());
	TEST_CHECK(!RemoveDirectoryA(directory));
	TEST_CHECK_EQ(ERROR_DIR_NOT_EMPTY, GetLastError());
	TEST_CHECK(GetFileAttributesA("wibo_remove_directory_fixture/entry.bin") != INVALID_FILE_ATTRIBUTES);
	TEST_CHECK(!RemoveDirectoryW(L"wibo_remove_directory_fixture/entry.bin"));
	TEST_CHECK_EQ(ERROR_DIRECTORY, GetLastError());
	TEST_CHECK(!RemoveDirectoryW(L"wibo_remove_directory_fixture/entry.bin/child"));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	TEST_CHECK(!RemoveDirectoryW(L"wibo_remove_directory_fixture/absent/child"));
	TEST_CHECK_EQ(ERROR_PATH_NOT_FOUND, GetLastError());
	TEST_CHECK(DeleteFileA("wibo_remove_directory_fixture/entry.bin"));
	TEST_CHECK(RemoveDirectoryA(directory));
	TEST_CHECK(!RemoveDirectoryW(L""));
	TEST_CHECK_EQ(ERROR_INVALID_NAME, GetLastError());
	TEST_CHECK(!RemoveDirectoryW(NULL));
	TEST_CHECK_EQ(ERROR_INVALID_NAME, GetLastError());
	WCHAR root[MAX_PATH];
	if (GetEnvironmentVariableW(L"WIBO_TEST_REMOVE_DIRECTORY_ROOT", root, MAX_PATH)) {
		TEST_CHECK(wcslen(root) + wcslen(L"/caf\u00e9-\U0001f680") < MAX_PATH);
		wcscat(root, L"/caf\u00e9-\U0001f680");
		TEST_CHECK(RemoveDirectoryW(root));
	}
	return EXIT_SUCCESS;
}
