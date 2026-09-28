#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winternl.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "test_assert.h"

typedef NTSTATUS(WINAPI *QueryInformationFn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
typedef NTSTATUS(WINAPI *QueryDirectoryFn)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
											   FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);

static void check_attributes(DWORD expected, DWORD attributes) {
	const DWORD mask = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY;
	TEST_CHECK_EQ(expected & mask, attributes & mask);
}

int main(void) {
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	TEST_CHECK(ntdll != NULL);
	FARPROC informationAddress = GetProcAddress(ntdll, "NtQueryInformationFile");
	FARPROC directoryAddress = GetProcAddress(ntdll, "NtQueryDirectoryFile");
	TEST_CHECK(informationAddress != NULL && directoryAddress != NULL);
	QueryInformationFn queryInformation;
	QueryDirectoryFn queryDirectory;
	memcpy(&queryInformation, &informationAddress, sizeof(queryInformation));
	memcpy(&queryDirectory, &directoryAddress, sizeof(queryDirectory));

	char temporary[MAX_PATH], directory[MAX_PATH], path[MAX_PATH];
	DWORD length = GetTempPathA(MAX_PATH, temporary);
	TEST_CHECK(length > 0 && length < MAX_PATH);
	TEST_CHECK(GetTempFileNameA(temporary, "nfa", 0, directory) != 0);
	TEST_CHECK(DeleteFileA(directory));
	TEST_CHECK(CreateDirectoryA(directory, NULL));
	int pathLength = snprintf(path, sizeof(path), "%s\\attribute.bin", directory);
	TEST_CHECK(pathLength > 0 && (size_t)pathLength < sizeof(path));

	HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
												 FILE_SHARE_DELETE, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	TEST_CHECK(SetFileAttributesA(path, FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY));
	const DWORD expected = GetFileAttributesA(path);
	TEST_CHECK(expected != INVALID_FILE_ATTRIBUTES && (expected & FILE_ATTRIBUTE_READONLY));

	IO_STATUS_BLOCK io;
	FILE_BASIC_INFORMATION basic;
	TEST_CHECK_EQ(0, queryInformation(file, &io, &basic, sizeof(basic), FileBasicInformation));
	check_attributes(expected, basic.FileAttributes);

	union {
		ULONGLONG alignment;
		char bytes[512];
	} all;
	TEST_CHECK_EQ(0, queryInformation(file, &io, all.bytes, sizeof(all.bytes), FileAllInformation));
	check_attributes(expected, ((FILE_ALL_INFORMATION *)all.bytes)->BasicInformation.FileAttributes);

	HANDLE parent = CreateFileA(directory, FILE_LIST_DIRECTORY | SYNCHRONIZE,
									 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
									 FILE_FLAG_BACKUP_SEMANTICS, NULL);
	TEST_CHECK(parent != INVALID_HANDLE_VALUE);
	WCHAR name[] = L"attribute.bin";
	UNICODE_STRING pattern = {sizeof(name) - sizeof(WCHAR), sizeof(name), name};
	union {
		ULONGLONG alignment;
		char bytes[512];
	} entry;
	TEST_CHECK_EQ(0, queryDirectory(parent, NULL, NULL, NULL, &io, entry.bytes, sizeof(entry.bytes),
									  FileDirectoryInformation, TRUE, &pattern, FALSE));
	check_attributes(expected, ((FILE_DIRECTORY_INFORMATION *)entry.bytes)->FileAttributes);

	TEST_CHECK(SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL));
	TEST_CHECK(CloseHandle(parent));
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(DeleteFileA(path));
	TEST_CHECK(RemoveDirectoryA(directory));
	return 0;
}
