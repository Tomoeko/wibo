#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

#include <stddef.h>

typedef BOOL(WINAPI *FileInformationFn)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);

enum { BUFFER_CAPACITY = 64 };

union GuardedBuffer {
	ULONGLONG alignment;
	BYTE bytes[16 + BUFFER_CAPACITY + 16];
};

struct CallResult {
	BOOL result;
	DWORD error;
};

_Static_assert(sizeof(FILE_STANDARD_INFO) == 24, "Standard file information occupies 24 bytes");
_Static_assert(offsetof(FILE_STANDARD_INFO, AllocationSize) == 0, "Allocation size offset");
_Static_assert(offsetof(FILE_STANDARD_INFO, EndOfFile) == 8, "End of file offset");
_Static_assert(offsetof(FILE_STANDARD_INFO, NumberOfLinks) == 16, "Link count offset");
_Static_assert(offsetof(FILE_STANDARD_INFO, DeletePending) == 20, "Delete state is a one-byte field");
_Static_assert(offsetof(FILE_STANDARD_INFO, Directory) == 21, "Directory state is a one-byte field");
_Static_assert(sizeof(((FILE_STANDARD_INFO *)0)->DeletePending) == 1, "Delete state width");
_Static_assert(sizeof(((FILE_STANDARD_INFO *)0)->Directory) == 1, "Directory state width");

static FileInformationFn get_file_information;

static void initialize_buffer(union GuardedBuffer *buffer) { memset(buffer, 0xa5, sizeof(*buffer)); }

static struct CallResult call_info(const char *label, HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, DWORD capacity,
								   union GuardedBuffer *output) {
	TEST_CHECK(capacity <= BUFFER_CAPACITY);
	SetLastError(0x4321);
	struct CallResult result;
	result.result = get_file_information(file, kind, output->bytes + 16, capacity);
	result.error = GetLastError();
	for (unsigned index = 0; index < sizeof(output->bytes); ++index)
		if (index < 16 || index >= 16 + capacity)
			TEST_CHECK_EQ(0xa5, output->bytes[index]);
	printf("%s: class=%u capacity=%lu result=%ld error=%lu\n", label, (unsigned)kind, (unsigned long)capacity,
		   (long)result.result, (unsigned long)result.error);
	return result;
}

static FILE_STANDARD_INFO standard_info(HANDLE file, LONGLONG expected_size, DWORD capacity, BOOL directory) {
	union GuardedBuffer output;
	initialize_buffer(&output);
	struct CallResult result = call_info("standard", file, FileStandardInfo, capacity, &output);
	TEST_CHECK(result.result);
	TEST_CHECK_EQ(0x4321, result.error);
	FILE_STANDARD_INFO info;
	memcpy(&info, output.bytes + 16, sizeof(info));
	TEST_CHECK_EQ(expected_size, info.EndOfFile.QuadPart);
	TEST_CHECK(info.AllocationSize.QuadPart >= expected_size);
	if (directory)
		TEST_CHECK_EQ(0, info.AllocationSize.QuadPart);
	TEST_CHECK(info.NumberOfLinks >= 1);
	TEST_CHECK_EQ(FALSE, info.DeletePending);
	TEST_CHECK_EQ(directory, info.Directory);
	for (unsigned index = 16 + sizeof(info); index < sizeof(output.bytes); ++index)
		TEST_CHECK_EQ(0xa5, output.bytes[index]);
	printf("standard-fields: allocation=%lld length=%lld links=%lu deleted=%u directory=%u\n",
		   info.AllocationSize.QuadPart, info.EndOfFile.QuadPart, (unsigned long)info.NumberOfLinks,
		   (unsigned)info.DeletePending, (unsigned)info.Directory);
	return info;
}

static DWORD failed_query(const char *label, HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, DWORD capacity) {
	union GuardedBuffer output;
	initialize_buffer(&output);
	struct CallResult result = call_info(label, file, kind, capacity, &output);
	TEST_CHECK(!result.result);
	for (unsigned index = 0; index < sizeof(output.bytes); ++index)
		TEST_CHECK_EQ(0xa5, output.bytes[index]);
	return result.error;
}

int main(void) {
	HMODULE module = GetModuleHandleW(L"kernel32.dll");
	TEST_CHECK(module != NULL);
	FARPROC exported = GetProcAddress(module, "GetFileInformationByHandleEx");
	_Static_assert(sizeof(exported) == sizeof(get_file_information), "Resolved function pointers have the same width");
	memcpy(&get_file_information, &exported, sizeof(get_file_information));
	TEST_CHECK(get_file_information != NULL);
	char temporary[MAX_PATH], filename[MAX_PATH];
	DWORD path_length = GetTempPathA(MAX_PATH, temporary);
	TEST_CHECK(path_length > 0 && path_length < MAX_PATH);
	TEST_CHECK(GetTempFileNameA(temporary, "FHI", 0, filename));
	const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
	HANDLE file =
		CreateFileA(filename, GENERIC_READ | GENERIC_WRITE, share, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	standard_info(file, 0, sizeof(FILE_STANDARD_INFO), FALSE);
	if (getenv("WIBO_EXPECT_FILE_INFORMATION_LIMITS"))
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, failed_query("unsupported-class", file, FileBasicInfo, BUFFER_CAPACITY));
	DWORD written;
	TEST_CHECK(WriteFile(file, "abcdef", 6, &written, NULL));
	TEST_CHECK_EQ(6, written);
	standard_info(file, 6, sizeof(FILE_STANDARD_INFO), FALSE);
	standard_info(file, 6, 32, FALSE);
	standard_info(file, 6, BUFFER_CAPACITY, FALSE);
	LARGE_INTEGER position;
	position.QuadPart = 2;
	TEST_CHECK(SetFilePointerEx(file, position, NULL, FILE_BEGIN));
	standard_info(file, 6, sizeof(FILE_STANDARD_INFO), FALSE);
	HANDLE duplicate;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	standard_info(duplicate, 6, sizeof(FILE_STANDARD_INFO), FALSE);
	HANDLE read_only = CreateFileA(filename, GENERIC_READ, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(read_only != INVALID_HANDLE_VALUE);
	standard_info(read_only, 6, sizeof(FILE_STANDARD_INFO), FALSE);
	TEST_CHECK(SetEndOfFile(file));
	standard_info(read_only, 2, sizeof(FILE_STANDARD_INFO), FALSE);
	standard_info(duplicate, 2, sizeof(FILE_STANDARD_INFO), FALSE);
	TEST_CHECK_EQ(ERROR_BAD_LENGTH, failed_query("short", file, FileStandardInfo, sizeof(FILE_STANDARD_INFO) - 1));
	TEST_CHECK_EQ(ERROR_BAD_LENGTH, failed_query("zero-capacity", file, FileStandardInfo, 0));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE,
				  failed_query("invalid-handle", INVALID_HANDLE_VALUE, FileStandardInfo, sizeof(FILE_STANDARD_INFO)));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, failed_query("invalid-class", file, (FILE_INFO_BY_HANDLE_CLASS)0x7fffffff,
														sizeof(FILE_STANDARD_INFO)));
	HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE,
				  failed_query("wrong-type", event, FileStandardInfo, sizeof(FILE_STANDARD_INFO)));
	TEST_CHECK(CloseHandle(event));
	TEST_CHECK(CloseHandle(read_only));
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE,
				  failed_query("stale-handle", file, FileStandardInfo, sizeof(FILE_STANDARD_INFO)));
	TEST_CHECK(DeleteFileA(filename));
	TEST_CHECK(CreateDirectoryA(filename, NULL));
	HANDLE directory =
		CreateFileA(filename, GENERIC_READ, share, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	TEST_CHECK(directory != INVALID_HANDLE_VALUE);
	standard_info(directory, 0, sizeof(FILE_STANDARD_INFO), TRUE);
	TEST_CHECK(CloseHandle(directory));
	TEST_CHECK(RemoveDirectoryA(filename));
	return 0;
}
