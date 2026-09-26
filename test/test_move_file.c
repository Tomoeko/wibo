#include "test_assert.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static void write_marker(const char *path, BYTE marker) {
	HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	DWORD transferred = 0;
	TEST_CHECK(WriteFile(file, &marker, 1, &transferred, NULL));
	TEST_CHECK_EQ(1, transferred);
	TEST_CHECK(CloseHandle(file));
}

static void check_marker(const char *path, BYTE expected) {
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	BYTE marker = 0;
	DWORD transferred = 0;
	TEST_CHECK(ReadFile(file, &marker, 1, &transferred, NULL));
	TEST_CHECK_EQ(1, transferred);
	TEST_CHECK_EQ(expected, marker);
	TEST_CHECK(CloseHandle(file));
}

static void check_cross_volume_error(void) {
	DWORD error = GetLastError();
	char runtime[2];
	if (GetEnvironmentVariableA("WIBO_FIXTURE_RUNTIME", runtime, sizeof(runtime)))
		TEST_CHECK_EQ(ERROR_NOT_SAME_DEVICE, error);
	else
		TEST_CHECK(error == ERROR_NOT_SAME_DEVICE || error == ERROR_ACCESS_DENIED);
}

int main(void) {
	char temporary[MAX_PATH], directory[MAX_PATH], source[MAX_PATH], destination[MAX_PATH];
	WCHAR wide_source[MAX_PATH], wide_destination[MAX_PATH];
	TEST_CHECK(GetTempPathA(MAX_PATH, temporary));
	TEST_CHECK(GetTempFileNameA(temporary, "mov", 0, directory));
	TEST_CHECK(DeleteFileA(directory));
	TEST_CHECK(CreateDirectoryA(directory, NULL));
	TEST_CHECK(snprintf(source, MAX_PATH, "%s/source.bin", directory) < MAX_PATH);
	TEST_CHECK(snprintf(destination, MAX_PATH, "%s/destination.bin", directory) < MAX_PATH);
	TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, source, -1, wide_source, MAX_PATH));
	TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, destination, -1, wide_destination, MAX_PATH));
	write_marker(source, 41);
	write_marker(destination, 42);
	TEST_CHECK(!MoveFileExW(wide_source, wide_destination, 0));
	TEST_CHECK_EQ(ERROR_ALREADY_EXISTS, GetLastError());
	check_marker(source, 41);
	check_marker(destination, 42);
	SetLastError(0x4321);
	TEST_CHECK(MoveFileExW(wide_source, wide_destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK_EQ(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(source));
	check_marker(destination, 41);
	TEST_CHECK(MoveFileExA(destination, source, MOVEFILE_COPY_ALLOWED));
	check_marker(source, 41);
	TEST_CHECK_EQ(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(destination));
	TEST_CHECK(MoveFileW(wide_source, wide_destination));
	check_marker(destination, 41);
	TEST_CHECK(!MoveFileExW(wide_source, wide_destination, MOVEFILE_REPLACE_EXISTING));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	TEST_CHECK(CreateDirectoryA(source, NULL));
	TEST_CHECK(!MoveFileExA(destination, source, MOVEFILE_REPLACE_EXISTING));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(RemoveDirectoryA(source));
	TEST_CHECK(DeleteFileA(destination));
	TEST_CHECK(CreateDirectoryA(source, NULL));
	TEST_CHECK(MoveFileExW(wide_source, wide_destination, 0));
	TEST_CHECK(GetFileAttributesA(destination) & FILE_ATTRIBUTE_DIRECTORY);
	TEST_CHECK(RemoveDirectoryA(destination));
	char other_directory[MAX_PATH], other_path[MAX_PATH];
	if (GetEnvironmentVariableA("WIBO_TEST_MOVE_OTHER_DIRECTORY", other_directory, MAX_PATH)) {
		TEST_CHECK(snprintf(other_path, MAX_PATH, "%s/move_%lu.bin", other_directory,
							(unsigned long)GetCurrentProcessId()) < MAX_PATH);
		write_marker(source, 51);
		TEST_CHECK(!MoveFileExA(source, other_path, 0));
		check_cross_volume_error();
		check_marker(source, 51);
		TEST_CHECK(MoveFileExA(source, other_path, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH));
		TEST_CHECK_EQ(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(source));
		check_marker(other_path, 51);
		write_marker(source, 52);
		TEST_CHECK(MoveFileExA(source, other_path, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING));
		TEST_CHECK_EQ(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(source));
		check_marker(other_path, 52);
		TEST_CHECK(DeleteFileA(other_path));
		TEST_CHECK(CreateDirectoryA(source, NULL));
		TEST_CHECK(!MoveFileExA(source, other_path, MOVEFILE_COPY_ALLOWED));
		check_cross_volume_error();
		TEST_CHECK(RemoveDirectoryA(source));
	}
	TEST_CHECK(RemoveDirectoryA(directory));
	return 0;
}
