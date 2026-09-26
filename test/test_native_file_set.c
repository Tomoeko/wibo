#include "test_assert.h"
#include <stdlib.h>
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS(WINAPI *SetInfo)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
static SetInfo setinfo;
static void set(HANDLE file, ULONG kind, LONGLONG value, ULONG length, ULONG expected) {
	IO_STATUS_BLOCK block;
	memset(&block, 0x71, sizeof(block));
	LARGE_INTEGER data;
	data.QuadPart = value;
	SetLastError(0x51);
	TEST_CHECK_U64_EQ(expected, (ULONG)setinfo(file, &block, &data, length, (FILE_INFORMATION_CLASS)kind));
	TEST_CHECK_U64_EQ(expected, (ULONG)block.Status);
	TEST_CHECK_EQ(0, block.Information);
	TEST_CHECK_EQ(0x51, GetLastError());
}
static LONGLONG position(HANDLE h) {
	LARGE_INTEGER zero, current;
	zero.QuadPart = 0;
	TEST_CHECK(SetFilePointerEx(h, zero, &current, FILE_CURRENT));
	return current.QuadPart;
}
int main(void) {
	setinfo = (SetInfo)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetInformationFile");
	TEST_CHECK(setinfo != NULL);
	const char *name = "wibo_native_set_fixture.tmp";
	HANDLE file = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
							  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	DWORD written;
	TEST_CHECK(WriteFile(file, "abcdef", 6, &written, NULL));
	TEST_CHECK_EQ(6, written);
	set(file, 14, 2, 8, 0);
	TEST_CHECK_EQ(2, position(file));
	HANDLE duplicate;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	set(duplicate, 14, 3, 8, 0);
	TEST_CHECK_EQ(3, position(file));
	HANDLE other = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	TEST_CHECK(other != INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(0, position(other));
	const LONGLONG wide = ((LONGLONG)1 << 33) + 7;
	set(file, 14, wide, 8, 0);
	TEST_CHECK_EQ(wide, position(duplicate));
	set(file, 20, 10, 8, 0);
	LARGE_INTEGER size;
	TEST_CHECK(GetFileSizeEx(file, &size));
	TEST_CHECK_EQ(10, size.QuadPart);
	TEST_CHECK_EQ(wide, position(file));
	BYTE bytes[10];
	DWORD read;
	TEST_CHECK(ReadFile(other, bytes, sizeof(bytes), &read, NULL));
	TEST_CHECK_EQ(10, read);
	TEST_CHECK(memcmp(bytes, "abcdef", 6) == 0);
	for (unsigned i = 6; i < 10; ++i)
		TEST_CHECK_EQ(0, bytes[i]);
	set(file, 20, 2, 8, 0);
	TEST_CHECK(GetFileSizeEx(file, &size));
	TEST_CHECK_EQ(2, size.QuadPart);
	set(file, 14, -1, 8, 0xc000000dU);
	TEST_CHECK_EQ(wide, position(file));
	set(file, 20, -1, 8, 0xc000000dU);
	TEST_CHECK(GetFileSizeEx(file, &size));
	TEST_CHECK_EQ(2, size.QuadPart);
	// The compatibility baseline reports argument three for a short native buffer.
	const ULONG shortStatus = getenv("WIBO_FIXTURE_RUNTIME") ? 0xc0000004U : 0xc00000f1U;
	set(file, 14, 0, 7, shortStatus);
	set(file, 20, 0, 7, shortStatus);
	set(file, 999, 0, 8, 0xc0000002U);
	// The baseline permits asynchronous position changes and differs on write access errors.
	const int runtime = getenv("WIBO_FIXTURE_RUNTIME") != NULL;
	set(other, 20, 0, 8, runtime ? 0xc0000022U : 0xc000000dU);
	TEST_CHECK(GetFileSizeEx(file, &size));
	TEST_CHECK_EQ(2, size.QuadPart);
	HANDLE async = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
							   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	TEST_CHECK(async != INVALID_HANDLE_VALUE);
	set(async, 14, 1, 8, runtime ? 0xc000000dU : 0);
	set(async, 20, 3, 8, 0);
	TEST_CHECK(GetFileSizeEx(file, &size));
	TEST_CHECK_EQ(3, size.QuadPart);
	TEST_CHECK(CloseHandle(async));
	TEST_CHECK(CloseHandle(other));
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(DeleteFileA(name));
	return 0;
}
