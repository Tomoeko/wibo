#define _WIN32_WINNT 0x0601
#include "test_assert.h"
#include <windows.h>

#include <stdio.h>
#include <string.h>

static const WCHAR replacedName[] = L"replaced-\u03a9.bin";
static const WCHAR replacementName[] = L"replacement-\u03a9.bin";
static const WCHAR backupName[] = L"backup-\u03a9.bin";

static void write_marker(const WCHAR *name, BYTE marker) {
	HANDLE file = CreateFileW(name, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	DWORD transferred = 0;
	TEST_CHECK(WriteFile(file, &marker, 1, &transferred, NULL));
	TEST_CHECK_EQ(1, transferred);
	TEST_CHECK(CloseHandle(file));
}

static void check_marker(const WCHAR *name, BYTE expected) {
	HANDLE file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	BYTE marker = 0;
	DWORD transferred = 0;
	TEST_CHECK(ReadFile(file, &marker, 1, &transferred, NULL));
	TEST_CHECK_EQ(1, transferred);
	TEST_CHECK_EQ(expected, marker);
	TEST_CHECK(CloseHandle(file));
}

static void check_missing(const WCHAR *name) { TEST_CHECK_EQ(INVALID_FILE_ATTRIBUTES, GetFileAttributesW(name)); }

int main(int argc, char **argv) {
	char original[MAX_PATH], temporary[MAX_PATH], directory[MAX_PATH];
	DWORD length = GetCurrentDirectoryA(MAX_PATH, original);
	TEST_CHECK(length && length < MAX_PATH);
	length = GetTempPathA(MAX_PATH, temporary);
	TEST_CHECK(length && length < MAX_PATH);
	TEST_CHECK(GetTempFileNameA(temporary, "rpl", 0, directory));
	TEST_CHECK(DeleteFileA(directory));
	TEST_CHECK(CreateDirectoryA(directory, NULL));
	TEST_CHECK(SetCurrentDirectoryA(directory));

	write_marker(replacedName, 'A');
	write_marker(replacementName, 'B');
	TEST_CHECK(ReplaceFileW(replacedName, replacementName, NULL, 0, NULL, NULL));
	check_marker(replacedName, 'B');
	check_missing(replacementName);
	TEST_CHECK(DeleteFileW(replacedName));

	write_marker(replacedName, 'C');
	write_marker(replacementName, 'D');
	TEST_CHECK(ReplaceFileW(replacedName, replacementName, backupName, 0, NULL, NULL));
	check_marker(replacedName, 'D');
	check_marker(backupName, 'C');
	check_missing(replacementName);
	TEST_CHECK(DeleteFileW(replacedName));
	TEST_CHECK(DeleteFileW(backupName));

	write_marker(replacementName, 'E');
	TEST_CHECK(!ReplaceFileW(replacedName, replacementName, backupName, 0, NULL, NULL));
	check_marker(replacementName, 'E');
	check_missing(replacedName);
	check_missing(backupName);
	write_marker(replacedName, 'F');
	TEST_CHECK(DeleteFileW(replacementName));
	TEST_CHECK(!ReplaceFileW(replacedName, replacementName, backupName, 0, NULL, NULL));
	check_marker(replacedName, 'F');
	check_missing(replacementName);
	check_missing(backupName);
	TEST_CHECK(DeleteFileW(replacedName));

	write_marker(replacedName, 'G');
	write_marker(replacementName, 'H');
	if (argc > 1 && strcmp(argv[1], "--unsupported") == 0) {
		SetLastError(0x4321);
		TEST_CHECK(!ReplaceFileW(replacedName, replacementName, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		check_marker(replacedName, 'G');
		check_marker(replacementName, 'H');
	} else if (argc > 1 && strcmp(argv[1], "--probe-backup-overwrite") == 0) {
		write_marker(backupName, 'I');
		SetLastError(0x4321);
		BOOL result = ReplaceFileW(replacedName, replacementName, backupName, 0, NULL, NULL);
		DWORD error = GetLastError();
		printf("backup_overwrite_result=%d error=%lu replaced=%d replacement=%d backup=%d\n", (int)result,
			   (unsigned long)error, GetFileAttributesW(replacedName) != INVALID_FILE_ATTRIBUTES,
			   GetFileAttributesW(replacementName) != INVALID_FILE_ATTRIBUTES,
			   GetFileAttributesW(backupName) != INVALID_FILE_ATTRIBUTES);
		TEST_CHECK(result);
		TEST_CHECK_EQ(0x4321, error);
		check_marker(replacedName, 'H');
		check_marker(backupName, 'G');
		check_missing(replacementName);
	} else {
		TEST_CHECK(ReplaceFileW(replacedName, replacementName, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL));
		check_marker(replacedName, 'H');
		check_missing(replacementName);
	}
	TEST_CHECK(DeleteFileW(replacedName));
	if (GetFileAttributesW(replacementName) != INVALID_FILE_ATTRIBUTES)
		TEST_CHECK(DeleteFileW(replacementName));
	if (GetFileAttributesW(backupName) != INVALID_FILE_ATTRIBUTES)
		TEST_CHECK(DeleteFileW(backupName));
	TEST_CHECK(SetCurrentDirectoryA(original));
	TEST_CHECK(RemoveDirectoryA(directory));
	puts("replace_file_ok");
	return 0;
}
