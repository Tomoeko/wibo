#include "test_assert.h"
#include <stddef.h>
#include <stdlib.h>
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS(WINAPI *query_fn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
typedef struct {
	LARGE_INTEGER creation, accessTime, writeTime, change;
	ULONG attributes, basicPadding;
	LARGE_INTEGER allocation, size;
	ULONG links;
	BYTE deleted, directory;
	USHORT standardPadding;
	LARGE_INTEGER id;
	ULONG eaSize, access;
	LARGE_INTEGER position;
	ULONG mode, alignment, nameLength;
	WCHAR name[1];
} FileAll;
_Static_assert(offsetof(FileAll, name) == 100, "Native file information layout");

int main(void) {
	query_fn query = (query_fn)(ULONG_PTR)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationFile");
	TEST_CHECK(query != NULL);
	const char *name = "wibo_file_all_fixture.tmp";
	HANDLE file = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
							  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	DWORD written;
	TEST_CHECK(WriteFile(file, "abcdef", 6, &written, NULL));
	LARGE_INTEGER two;
	two.QuadPart = 2;
	TEST_CHECK(SetFilePointerEx(file, two, NULL, FILE_BEGIN));
	union {
		ULONGLONG align;
		BYTE bytes[2048];
	} storage;
	memset(storage.bytes, 0xcc, sizeof(storage.bytes));
	IO_STATUS_BLOCK io;
	SetLastError(0x71);
	TEST_CHECK_EQ(0, query(file, &io, storage.bytes, sizeof(storage.bytes) - 8, (FILE_INFORMATION_CLASS)18));
	TEST_CHECK_EQ(0x71, GetLastError());
	FileAll *info = (FileAll *)storage.bytes;
	TEST_CHECK_EQ(0, io.Status);
	TEST_CHECK_EQ(100 + info->nameLength, io.Information);
	TEST_CHECK_EQ(6, info->size.QuadPart);
	TEST_CHECK_EQ(2, info->position.QuadPart);
	TEST_CHECK_EQ(0, info->directory);
	TEST_CHECK_EQ(0, info->deleted);
	TEST_CHECK(!(info->attributes & FILE_ATTRIBUTE_DIRECTORY));
	// The compatibility baseline omits access/mode and reports word alignment;
	// the runtime reports its granted rights, synchronous mode and byte alignment.
	const BOOL runtime = getenv("WIBO_FIXTURE_RUNTIME") != NULL;
	TEST_CHECK_EQ(runtime ? FILE_GENERIC_READ | FILE_GENERIC_WRITE : 0, info->access);
	TEST_CHECK_EQ(runtime ? 0x20 : 0, info->mode);
	TEST_CHECK_EQ(runtime ? 0 : 1, info->alignment);
	TEST_CHECK_EQ(0, info->eaSize);
	TEST_CHECK(info->nameLength > 4 && info->nameLength % sizeof(WCHAR) == 0);
	BY_HANDLE_FILE_INFORMATION handleInfo;
	TEST_CHECK(GetFileInformationByHandle(file, &handleInfo));
	TEST_CHECK_EQ(handleInfo.dwFileAttributes, info->attributes);
	TEST_CHECK_U64_EQ(((ULONGLONG)handleInfo.ftCreationTime.dwHighDateTime << 32) | handleInfo.ftCreationTime.dwLowDateTime,
					  info->creation.QuadPart);
	TEST_CHECK_U64_EQ(((ULONGLONG)handleInfo.ftLastAccessTime.dwHighDateTime << 32) | handleInfo.ftLastAccessTime.dwLowDateTime,
					  info->accessTime.QuadPart);
	TEST_CHECK_U64_EQ(((ULONGLONG)handleInfo.ftLastWriteTime.dwHighDateTime << 32) | handleInfo.ftLastWriteTime.dwLowDateTime,
					  info->writeTime.QuadPart);
	TEST_CHECK_U64_EQ(((ULONGLONG)handleInfo.nFileIndexHigh << 32) | handleInfo.nFileIndexLow, info->id.QuadPart);
	TEST_CHECK_EQ(handleInfo.nNumberOfLinks, info->links);
	for (unsigned i = sizeof(storage.bytes) - 8; i < sizeof(storage.bytes); ++i)
		TEST_CHECK_EQ(0xcc, storage.bytes[i]);
	BYTE prefix[104];
	memset(prefix, 0xcc, sizeof(prefix));
	TEST_CHECK_U64_EQ(0x80000005U, (ULONG)query(file, &io, prefix, sizeof(prefix), (FILE_INFORMATION_CLASS)18));
	TEST_CHECK_EQ(sizeof(prefix), io.Information);
	TEST_CHECK(memcmp(prefix, storage.bytes, sizeof(prefix)) == 0);
	memset(prefix, 0xcc, sizeof(prefix));
	TEST_CHECK_U64_EQ(0xC0000004U, (ULONG)query(file, &io, prefix, 99, (FILE_INFORMATION_CLASS)18));
	for (unsigned i = 0; i < sizeof(prefix); ++i)
		TEST_CHECK_EQ(0xcc, prefix[i]);
	HANDLE async = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							   FILE_FLAG_OVERLAPPED, NULL);
	TEST_CHECK(async != INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(0, query(async, &io, storage.bytes, sizeof(storage.bytes), (FILE_INFORMATION_CLASS)18));
	TEST_CHECK_EQ(0, info->mode);
	TEST_CHECK_EQ(0, info->position.QuadPart);
	TEST_CHECK(CloseHandle(async));
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(DeleteFileA(name));
	return 0;
}
