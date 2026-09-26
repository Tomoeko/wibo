#include "test_assert.h"
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winternl.h>

#define ST_SUCCESS ((NTSTATUS)0)
#define ST_OVERFLOW ((NTSTATUS)0x80000005)
#define ST_NO_MORE ((NTSTATUS)0x80000006)
#define ST_NO_SUCH ((NTSTATUS)0xc000000f)
#define ST_LENGTH ((NTSTATUS)0xc0000004)

typedef struct {
	ULONG NextEntryOffset, FileIndex;
	LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
	LARGE_INTEGER EndOfFile, AllocationSize;
	ULONG FileAttributes, FileNameLength;
	WCHAR FileName[1];
} DirectoryInformation;

typedef NTSTATUS(WINAPI *QueryDirectoryFn)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
										   FILE_INFORMATION_CLASS, BOOLEAN, UNICODE_STRING *, BOOLEAN);
static QueryDirectoryFn queryDirectory;
static const char *directory = "wibo_ntquerydirectory_fixture";
static const WCHAR *names[] = {L"alpha.bin", L"long-name.bin", L"plain", L"subdir"};

static HANDLE open_directory(const char *path) {
	HANDLE result =
		CreateFileA(path, FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	TEST_CHECK(result != INVALID_HANDLE_VALUE);
	return result;
}

static NTSTATUS query(HANDLE handle, void *buffer, ULONG length, BOOLEAN single, const WCHAR *pattern, BOOLEAN restart,
					  IO_STATUS_BLOCK *io) {
	UNICODE_STRING search;
	if (pattern) {
		search.Length = (USHORT)(wcslen(pattern) * sizeof(WCHAR));
		search.MaximumLength = search.Length;
		search.Buffer = (WCHAR *)pattern;
	}
	memset(io, 0xa5, sizeof(*io));
	return queryDirectory(handle, NULL, NULL, NULL, io, buffer, length, FileDirectoryInformation, single,
						  pattern ? &search : NULL, restart);
}

static int name_is(const DirectoryInformation *entry, const WCHAR *name) {
	size_t length = wcslen(name) * sizeof(WCHAR);
	return entry->FileNameLength == length && memcmp(entry->FileName, name, length) == 0;
}

static void check_records(void *buffer, ULONG_PTR bytes, unsigned *seen, unsigned *count) {
	ULONG_PTR offset = 0;
	for (;;) {
		TEST_CHECK(bytes - offset >= offsetof(DirectoryInformation, FileName));
		DirectoryInformation *entry = (DirectoryInformation *)((char *)buffer + offset);
		TEST_CHECK(entry->FileNameLength % sizeof(WCHAR) == 0);
		TEST_CHECK(bytes - offset >= offsetof(DirectoryInformation, FileName) + entry->FileNameLength);
		unsigned bit = 0;
		if (name_is(entry, L"."))
			bit = 1;
		else if (name_is(entry, L".."))
			bit = 2;
		else {
			for (unsigned i = 0; i != 4; ++i)
				if (name_is(entry, names[i]))
					bit = 4u << i;
		}
		TEST_CHECK(bit != 0);
		TEST_CHECK((*seen & bit) == 0);
		*seen |= bit;
		++*count;
		TEST_CHECK(entry->CreationTime.QuadPart != 0);
		TEST_CHECK(entry->LastWriteTime.QuadPart != 0);
		if (bit == 4 || bit == 8 || bit == 16) {
			TEST_CHECK_EQ(3, entry->EndOfFile.QuadPart);
			TEST_CHECK((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
		} else
			TEST_CHECK((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
		if (!entry->NextEntryOffset)
			break;
		TEST_CHECK(entry->NextEntryOffset % 8 == 0);
		TEST_CHECK(entry->NextEntryOffset >= offsetof(DirectoryInformation, FileName) + entry->FileNameLength);
		TEST_CHECK(entry->NextEntryOffset < bytes - offset);
		offset += entry->NextEntryOffset;
	}
}

static void test_enumeration(BOOLEAN single, ULONG capacity) {
	HANDLE handle = open_directory(directory), duplicate;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	union {
		ULONGLONG alignment;
		char bytes[4097];
	} buffer;
	IO_STATUS_BLOCK io;
	unsigned seen = 0, count = 0, calls = 0;
	for (;;) {
		memset(buffer.bytes, 0xcc, sizeof(buffer.bytes));
		NTSTATUS status = query(calls % 2 ? duplicate : handle, buffer.bytes, capacity, single, NULL, FALSE, &io);
		++calls;
		if (status == ST_NO_MORE)
			break;
		TEST_CHECK_EQ(ST_SUCCESS, status);
		TEST_CHECK_EQ(ST_SUCCESS, io.Status);
		TEST_CHECK(io.Information > 0 && io.Information <= capacity);
		TEST_CHECK_EQ((unsigned char)0xcc, (unsigned char)buffer.bytes[io.Information]);
		unsigned before = count;
		check_records(buffer.bytes, io.Information, &seen, &count);
		if (single)
			TEST_CHECK_EQ(before + 1, count);
		TEST_CHECK(calls < 20);
	}
	TEST_CHECK_EQ(63, seen);
	TEST_CHECK_EQ(6, count);
	TEST_CHECK_EQ(ST_SUCCESS, query(duplicate, buffer.bytes, sizeof(buffer.bytes), FALSE, NULL, TRUE, &io));
	seen = count = 0;
	check_records(buffer.bytes, io.Information, &seen, &count);
	TEST_CHECK_EQ(63, seen);
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK(CloseHandle(handle));
}

static void test_search(void) {
	HANDLE handle = open_directory(directory);
	union {
		ULONGLONG alignment;
		char bytes[4097];
	} buffer;
	IO_STATUS_BLOCK io;
	TEST_CHECK_EQ(ST_SUCCESS, query(handle, buffer.bytes, sizeof(buffer.bytes), TRUE, L"*.BIN", FALSE, &io));
	DirectoryInformation first = *(DirectoryInformation *)buffer.bytes;
	WCHAR firstName[64];
	memcpy(firstName, ((DirectoryInformation *)buffer.bytes)->FileName, first.FileNameLength);
	firstName[first.FileNameLength / sizeof(WCHAR)] = 0;
	TEST_CHECK_EQ(0, first.NextEntryOffset);
	TEST_CHECK_EQ(ST_SUCCESS, query(handle, buffer.bytes, sizeof(buffer.bytes), TRUE, L"plain", FALSE, &io));
	TEST_CHECK(!name_is((DirectoryInformation *)buffer.bytes, firstName));
	TEST_CHECK(name_is((DirectoryInformation *)buffer.bytes, L"alpha.bin") ||
			   name_is((DirectoryInformation *)buffer.bytes, L"long-name.bin"));
	TEST_CHECK_EQ(ST_NO_MORE, query(handle, buffer.bytes, sizeof(buffer.bytes), TRUE, NULL, FALSE, &io));
	TEST_CHECK_EQ(ST_SUCCESS, query(handle, buffer.bytes, sizeof(buffer.bytes), TRUE, NULL, TRUE, &io));
	TEST_CHECK(name_is((DirectoryInformation *)buffer.bytes, firstName));
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		TEST_CHECK_EQ(ST_SUCCESS, query(handle, buffer.bytes, sizeof(buffer.bytes), TRUE, L"plain", TRUE, &io));
		TEST_CHECK(name_is((DirectoryInformation *)buffer.bytes, firstName));
	}
	TEST_CHECK(CloseHandle(handle));
	handle = open_directory(directory);
	TEST_CHECK_EQ(ST_NO_SUCH, query(handle, buffer.bytes, sizeof(buffer.bytes), FALSE, L"absent", FALSE, &io));
	TEST_CHECK_EQ(ST_NO_MORE, query(handle, buffer.bytes, sizeof(buffer.bytes), FALSE, NULL, FALSE, &io));
	TEST_CHECK(CloseHandle(handle));
	handle = open_directory(directory);
	TEST_CHECK_EQ(ST_SUCCESS, query(handle, buffer.bytes, sizeof(buffer.bytes), FALSE, L"*.*", FALSE, &io));
	unsigned seen = 0, count = 0;
	check_records(buffer.bytes, io.Information, &seen, &count);
	TEST_CHECK_EQ(15, seen);
	TEST_CHECK(CloseHandle(handle));
}

static void test_buffers(void) {
	HANDLE handle = open_directory(directory);
	union {
		ULONGLONG alignment;
		char bytes[256];
	} buffer;
	IO_STATUS_BLOCK io;
	memset(buffer.bytes, 0xcc, sizeof(buffer.bytes));
	ULONG small = sizeof(DirectoryInformation);
	NTSTATUS status = query(handle, buffer.bytes, small, TRUE, L"long-name.bin", FALSE, &io);
	TEST_CHECK_EQ(ST_OVERFLOW, status);
	TEST_CHECK_EQ(small, io.Information);
	TEST_CHECK_EQ(wcslen(L"long-name.bin") * sizeof(WCHAR), ((DirectoryInformation *)buffer.bytes)->FileNameLength);
	TEST_CHECK_EQ(L'l', ((DirectoryInformation *)buffer.bytes)->FileName[0]);
	TEST_CHECK_EQ((unsigned char)0xcc, (unsigned char)buffer.bytes[small]);
	status = query(handle, buffer.bytes, sizeof(buffer.bytes), TRUE, NULL, FALSE, &io);
	TEST_CHECK_EQ(ST_NO_MORE, status);
	TEST_CHECK_EQ(0, io.Information);
	TEST_CHECK(CloseHandle(handle));
	handle = open_directory(directory);
	status = query(handle, buffer.bytes, 63, TRUE, NULL, FALSE, &io);
	TEST_CHECK_EQ(ST_LENGTH, status);
	TEST_CHECK_U64_EQ((ULONG_PTR) ~(ULONG_PTR)0 / 0xff * 0xa5, io.Information);
	TEST_CHECK(CloseHandle(handle));
	handle = open_directory(directory);
	TEST_CHECK_EQ(ST_SUCCESS, query(handle, buffer.bytes, sizeof(buffer.bytes), TRUE, L"*.bin", FALSE, &io));
	status = query(handle, buffer.bytes, small, TRUE, NULL, FALSE, &io);
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		// A later short buffer must preserve the pending entry for a larger buffer.
		TEST_CHECK_EQ(ST_SUCCESS, status);
		TEST_CHECK_EQ(0, io.Information);
		TEST_CHECK_EQ(ST_SUCCESS, query(handle, buffer.bytes, sizeof(buffer.bytes), TRUE, NULL, FALSE, &io));
	} else {
		// This baseline returns and consumes a partial entry on later calls too.
		TEST_CHECK_EQ(ST_OVERFLOW, status);
		TEST_CHECK_EQ(small, io.Information);
		TEST_CHECK_EQ(ST_NO_MORE, query(handle, buffer.bytes, sizeof(buffer.bytes), TRUE, NULL, FALSE, &io));
	}
	TEST_CHECK(CloseHandle(handle));
}

static void test_invalid_requests(void) {
	union {
		ULONGLONG alignment;
		char bytes[256];
	} buffer;
	IO_STATUS_BLOCK io;
	NTSTATUS status = query((HANDLE)0x1234, buffer.bytes, sizeof(buffer.bytes), FALSE, NULL, FALSE, &io);
	TEST_CHECK_EQ((NTSTATUS)0xc0000008, status);
	HANDLE file = CreateFileA("wibo_ntquerydirectory_fixture/alpha.bin", GENERIC_READ, 7, NULL, OPEN_EXISTING, 0, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	status = query(file, buffer.bytes, sizeof(buffer.bytes), FALSE, NULL, FALSE, &io);
	TEST_CHECK_EQ((NTSTATUS)0xc000000d, status);
	TEST_CHECK(CloseHandle(file));
	HANDLE handle = CreateFileA(directory, FILE_READ_ATTRIBUTES | SYNCHRONIZE, 7, NULL, OPEN_EXISTING,
								FILE_FLAG_BACKUP_SEMANTICS, NULL);
	TEST_CHECK(handle != INVALID_HANDLE_VALUE);
	status = query(handle, buffer.bytes, sizeof(buffer.bytes), FALSE, NULL, FALSE, &io);
	TEST_CHECK_EQ((NTSTATUS)0xc0000022, status);
	TEST_CHECK(CloseHandle(handle));
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		handle = open_directory(directory);
		TEST_CHECK_EQ((NTSTATUS)0xc00000bb,
					  query(handle, buffer.bytes, sizeof(buffer.bytes), FALSE, L"<.bin", FALSE, &io));
		TEST_CHECK_EQ((NTSTATUS)0xc00000bb,
					  query(handle, buffer.bytes, sizeof(buffer.bytes), FALSE, L"caf\u00e9", FALSE, &io));
		TEST_CHECK_EQ((NTSTATUS)0xc00000bb,
					  queryDirectory(handle, NULL, NULL, NULL, &io, buffer.bytes, sizeof(buffer.bytes),
									 FileFullDirectoryInformation, FALSE, NULL, FALSE));
		TEST_CHECK_EQ((NTSTATUS)0xc00000bb,
					  queryDirectory(handle, (HANDLE)1, NULL, NULL, &io, buffer.bytes, sizeof(buffer.bytes),
									 FileDirectoryInformation, FALSE, NULL, FALSE));
		TEST_CHECK(CloseHandle(handle));
	}
}

static void test_utf8_directory(void) {
	const char *path = getenv("WIBO_TEST_DIRECTORY_UTF8");
	if (!path)
		return;
	HANDLE handle = open_directory(path);
	union {
		ULONGLONG alignment;
		char bytes[4097];
	} buffer;
	IO_STATUS_BLOCK io;
	TEST_CHECK_EQ(ST_SUCCESS, query(handle, buffer.bytes, sizeof(buffer.bytes), FALSE, NULL, FALSE, &io));
	size_t offset = 0;
	int found = 0;
	for (;;) {
		DirectoryInformation *entry = (DirectoryInformation *)(buffer.bytes + offset);
		if (name_is(entry, L"caf\u00e9-\U0001f680.bin"))
			found = 1;
		if (!entry->NextEntryOffset)
			break;
		offset += entry->NextEntryOffset;
		TEST_CHECK(offset < io.Information);
	}
	TEST_CHECK(found);
	TEST_CHECK(CloseHandle(handle));
}

int main(void) {
	HMODULE module = GetModuleHandleW(L"ntdll.dll");
	TEST_CHECK(module != NULL);
	FARPROC proc = GetProcAddress(module, "NtQueryDirectoryFile");
	TEST_CHECK(proc != NULL);
	memcpy(&queryDirectory, &proc, sizeof(queryDirectory));
	TEST_CHECK_EQ(64, offsetof(DirectoryInformation, FileName));
	TEST_CHECK(CreateDirectoryA(directory, NULL));
	for (unsigned i = 0; i != 3; ++i) {
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s/%ls", directory, names[i]);
		HANDLE file = CreateFileA(path, GENERIC_WRITE, 7, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		TEST_CHECK(file != INVALID_HANDLE_VALUE);
		DWORD written;
		TEST_CHECK(WriteFile(file, "abc", 3, &written, NULL));
		TEST_CHECK_EQ(3, written);
		TEST_CHECK(CloseHandle(file));
	}
	char subdir[MAX_PATH];
	snprintf(subdir, sizeof(subdir), "%s/subdir", directory);
	TEST_CHECK(CreateDirectoryA(subdir, NULL));
	test_enumeration(FALSE, 4096);
	test_enumeration(FALSE, 160);
	test_enumeration(TRUE, 4096);
	test_search();
	test_buffers();
	test_invalid_requests();
	test_utf8_directory();
	TEST_CHECK(RemoveDirectoryA(subdir));
	for (unsigned i = 0; i != 3; ++i) {
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s/%ls", directory, names[i]);
		TEST_CHECK(DeleteFileA(path));
	}
	TEST_CHECK(RemoveDirectoryA(directory));
	return EXIT_SUCCESS;
}
