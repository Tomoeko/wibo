#include "test_assert.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static HANDLE open_file(const char *path, DWORD flags) {
	HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
							  flags, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	return file;
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "child") == 0) {
		HANDLE file = open_file(argv[2], FILE_ATTRIBUTE_NORMAL);
		TEST_CHECK(!LockFile(file, 8, 0, 16, 0));
		TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
		TEST_CHECK(CloseHandle(file));
		return 0;
	}
	char directory[MAX_PATH], path[MAX_PATH];
	TEST_CHECK(GetTempPathA(MAX_PATH, directory));
	TEST_CHECK(GetTempFileNameA(directory, "lck", 0, path));
	HANDLE first = open_file(path, FILE_ATTRIBUTE_NORMAL), second = open_file(path, FILE_ATTRIBUTE_NORMAL);
	BYTE data[32] = {0};
	DWORD transferred = 0;
	TEST_CHECK(WriteFile(first, data, sizeof(data), &transferred, NULL));
	TEST_CHECK_EQ(sizeof(data), transferred);
	SetLastError(0x4321);
	TEST_CHECK(LockFile(first, 8, 0, 16, 0));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK(!LockFile(second, 8, 0, 16, 0));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(!LockFile(first, 8, 0, 16, 0));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());

	char module[MAX_PATH], command[2 * MAX_PATH + 32];
	TEST_CHECK(GetModuleFileNameA(NULL, module, MAX_PATH));
	snprintf(command, sizeof(command), "\"%s\" child \"%s\"", module, path);
	STARTUPINFOA startup = {0};
	PROCESS_INFORMATION process = {0};
	startup.cb = sizeof(startup);
	TEST_CHECK(CreateProcessA(module, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(process.hProcess, 5000));
	DWORD exit_code = 1;
	TEST_CHECK(GetExitCodeProcess(process.hProcess, &exit_code));
	TEST_CHECK_EQ(0, exit_code);
	TEST_CHECK(CloseHandle(process.hThread));
	TEST_CHECK(CloseHandle(process.hProcess));

	OVERLAPPED region = {0};
	region.Offset = 8;
	char runtime[2];
	if (GetEnvironmentVariableA("WIBO_FIXTURE_RUNTIME", runtime, sizeof(runtime))) {
		TEST_CHECK(!ReadFile(second, data, 1, &transferred, &region));
		TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	}
	TEST_CHECK(!UnlockFile(first, 8, 0, 8, 0));
	TEST_CHECK(UnlockFileEx(first, 0, 16, 0, &region));
	TEST_CHECK(LockFileEx(first, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &region));
	TEST_CHECK(UnlockFile(first, 8, 0, 16, 0));
	TEST_CHECK(LockFile(first, 0x20, 1, 0x40, 1));
	TEST_CHECK(!LockFile(second, 0x30, 2, 1, 0));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(UnlockFile(first, 0x20, 1, 0x40, 1));
	TEST_CHECK(LockFile(second, 0x30, 2, 1, 0));
	TEST_CHECK(UnlockFile(second, 0x30, 2, 1, 0));
	TEST_CHECK(!LockFile(INVALID_HANDLE_VALUE, 0, 0, 1, 0));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(!UnlockFile(INVALID_HANDLE_VALUE, 0, 0, 1, 0));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	HANDLE asynchronous = open_file(path, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
	TEST_CHECK(LockFile(asynchronous, 8, 0, 16, 0));
	TEST_CHECK(!LockFile(second, 8, 0, 16, 0));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(UnlockFile(asynchronous, 8, 0, 16, 0));
	TEST_CHECK(CloseHandle(asynchronous));
	TEST_CHECK(CloseHandle(first));
	TEST_CHECK(CloseHandle(second));
	TEST_CHECK(DeleteFileA(path));
	return 0;
}
