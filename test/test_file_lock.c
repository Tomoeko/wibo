#include "test_assert.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static HANDLE open_file(const char *path) {
	HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	return file;
}

static OVERLAPPED at_offset(ULONGLONG offset) {
	OVERLAPPED operation = {0};
	operation.Offset = (DWORD)offset;
	operation.OffsetHigh = (DWORD)(offset >> 32);
	return operation;
}

static DWORD WINAPI wait_for_lock(LPVOID parameter) {
	HANDLE *handles = parameter;
	OVERLAPPED operation = at_offset(8);
	TEST_CHECK(SetEvent(handles[1]));
	TEST_CHECK(LockFileEx(handles[0], LOCKFILE_EXCLUSIVE_LOCK, 0, 16, 0, &operation));
	TEST_CHECK(UnlockFileEx(handles[0], 0, 16, 0, &operation));
	return 0;
}

static void check_child(const char *path) {
	char module[MAX_PATH], command[MAX_PATH * 2 + 32];
	TEST_CHECK(GetModuleFileNameA(NULL, module, MAX_PATH) > 0);
	TEST_CHECK(snprintf(command, sizeof(command), "\"%s\" child \"%s\"", module, path) < (int)sizeof(command));
	STARTUPINFOA startup = {0};
	PROCESS_INFORMATION process = {0};
	startup.cb = sizeof(startup);
	TEST_CHECK(CreateProcessA(module, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(process.hProcess, 5000));
	DWORD exit_code = 1;
	TEST_CHECK(GetExitCodeProcess(process.hProcess, &exit_code));
	TEST_CHECK_EQ(0, exit_code);
	TEST_CHECK(CloseHandle(process.hProcess));
	TEST_CHECK(CloseHandle(process.hThread));
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "child") == 0) {
		HANDLE file = open_file(argv[2]);
		OVERLAPPED region = at_offset(8);
		TEST_CHECK(!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &region));
		TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
		TEST_CHECK(CloseHandle(file));
		return 0;
	}
	char directory[MAX_PATH], path[MAX_PATH];
	TEST_CHECK(GetTempPathA(MAX_PATH, directory) > 0);
	TEST_CHECK(snprintf(path, MAX_PATH, "%srange_lock_%lx_%lx.tmp", directory, GetCurrentProcessId(), GetTickCount()) <
			   MAX_PATH);
	HANDLE first = open_file(path), second = open_file(path);
	OVERLAPPED region = at_offset(8), other = at_offset(64);
	DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY;
	BYTE data[128] = {0};
	DWORD transferred = 0;
	TEST_CHECK(WriteFile(first, data, sizeof(data), &transferred, NULL));
	TEST_CHECK_EQ(sizeof(data), transferred);
	SetLastError(0x4321);
	TEST_CHECK(LockFileEx(first, flags, 0, 16, 0, &region));
	TEST_CHECK_EQ(0x4321, GetLastError());
	check_child(path);
	TEST_CHECK(!ReadFile(second, data, 1, &transferred, &region));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(!WriteFile(second, data, 1, &transferred, &region));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(ReadFile(first, data, 1, &transferred, &region));
	TEST_CHECK(WriteFile(first, data, 1, &transferred, &region));
	TEST_CHECK(!LockFileEx(second, flags, 0, 16, 0, &region));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(!LockFileEx(first, flags, 0, 16, 0, &region));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(LockFileEx(second, flags, 0, 8, 0, &other));
	TEST_CHECK(!UnlockFileEx(first, 0, 8, 0, &region));
	HANDLE handles[2] = {second, CreateEventA(NULL, FALSE, FALSE, NULL)};
	TEST_CHECK(handles[1] != NULL);
	HANDLE worker = CreateThread(NULL, 0, wait_for_lock, handles, 0, NULL);
	TEST_CHECK(worker != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(handles[1], 5000));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(worker, 20));
	TEST_CHECK(UnlockFileEx(first, 0, 16, 0, &region));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(worker, 5000));
	TEST_CHECK(CloseHandle(worker));
	TEST_CHECK(CloseHandle(handles[1]));
	TEST_CHECK(UnlockFileEx(second, 0, 8, 0, &other));
	TEST_CHECK(LockFileEx(first, LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &region));
	TEST_CHECK(LockFileEx(first, LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &region));
	TEST_CHECK(LockFileEx(second, LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &region));
	TEST_CHECK(!WriteFile(first, data, 1, &transferred, &region));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(ReadFile(second, data, 1, &transferred, &region));
	TEST_CHECK(UnlockFileEx(first, 0, 16, 0, &region));
	TEST_CHECK(!LockFileEx(second, flags, 0, 16, 0, &region));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(UnlockFileEx(first, 0, 16, 0, &region));
	TEST_CHECK(UnlockFileEx(second, 0, 16, 0, &region));
	OVERLAPPED overlap = at_offset(16);
	TEST_CHECK(LockFileEx(first, LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &region));
	TEST_CHECK(LockFileEx(first, LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &overlap));
	TEST_CHECK(UnlockFileEx(first, 0, 16, 0, &region));
	TEST_CHECK(LockFileEx(second, flags, 0, 8, 0, &region));
	TEST_CHECK(!LockFileEx(second, flags, 0, 8, 0, &overlap));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(UnlockFileEx(first, 0, 16, 0, &overlap));
	TEST_CHECK(LockFileEx(second, flags, 0, 8, 0, &overlap));
	TEST_CHECK(UnlockFileEx(second, 0, 8, 0, &region));
	TEST_CHECK(UnlockFileEx(second, 0, 8, 0, &overlap));
	region = at_offset(0x100000010ULL);
	TEST_CHECK(LockFileEx(first, flags, 0, 16, 0, &region));
	TEST_CHECK(!LockFileEx(second, flags, 0, 16, 0, &region));
	TEST_CHECK_EQ(ERROR_LOCK_VIOLATION, GetLastError());
	TEST_CHECK(CloseHandle(first));
	TEST_CHECK(LockFileEx(second, flags, 0, 16, 0, &region));
	TEST_CHECK(UnlockFileEx(second, 0, 16, 0, &region));
	TEST_CHECK(CloseHandle(second));
	TEST_CHECK(DeleteFileA(path));
	return 0;
}
