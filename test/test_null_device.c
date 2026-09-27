#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "test_assert.h"

static const DWORD seed = 0x4321;

struct CountOutput {
	DWORD before;
	DWORD value;
	DWORD after;
};

static struct CountOutput countOutput(void) {
	struct CountOutput output = {0x11223344, 0x5555, 0x55667788};
	return output;
}

static void checkCount(const struct CountOutput *output, DWORD expected) {
	TEST_CHECK_EQ(0x11223344, output->before);
	TEST_CHECK_EQ(expected, output->value);
	TEST_CHECK_EQ(0x55667788, output->after);
}

static HANDLE openDevice(const char *name, DWORD access, DWORD disposition, DWORD flags, SECURITY_ATTRIBUTES *security,
						 int wide, DWORD expectedError) {
	HANDLE handle;
	DWORD handleFlags = 0x5555;
	SetLastError(seed);
	if (wide) {
		WCHAR wideName[MAX_PATH];
		size_t i, length = strlen(name);
		TEST_CHECK(length < MAX_PATH);
		for (i = 0; i <= length; ++i)
			wideName[i] = (WCHAR)(unsigned char)name[i];
		handle = CreateFileW(wideName, access, 0, security, disposition, flags, NULL);
	} else {
		handle = CreateFileA(name, access, 0, security, disposition, flags, NULL);
	}
	TEST_CHECK(handle != INVALID_HANDLE_VALUE && handle != NULL);
	TEST_CHECK_EQ(expectedError, GetLastError());
	SetLastError(seed);
	TEST_CHECK_EQ(FILE_TYPE_CHAR, GetFileType(handle));
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	TEST_CHECK(GetHandleInformation(handle, &handleFlags));
	TEST_CHECK_EQ(security && security->bInheritHandle ? HANDLE_FLAG_INHERIT : 0, handleFlags);
	TEST_CHECK_EQ(seed, GetLastError());
	return handle;
}

static void checkAliases(void) {
	static const char *aliases[] = {
		"NUL",	"nul",	  "NuL",	  "NUL:",	"NUL.txt", "NUL.tar.gz", "NUL ",
		"NUL.", "NUL...", "NUL .txt", ".\\NUL", "./NUL",   "\\\\.\\NUL", "\\\\?\\NUL",
	};
	static const char *ordinaryNames[] = {
		"\\\\.\\NUL.txt", "\\\\?\\NUL.txt", "NULx", " NUL", "NUL\\", "\\\\.\\NUL:", "\\\\?\\NUL:",
	};
	unsigned i;
	int wide;
	for (wide = 0; wide < 2; ++wide) {
		for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
			HANDLE handle = openDevice(aliases[i], GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0, NULL, wide, 0);
			TEST_CHECK(CloseHandle(handle));
		}
		for (i = 0; i < sizeof(ordinaryNames) / sizeof(ordinaryNames[0]); ++i) {
			HANDLE handle;
			SetLastError(seed);
			if (wide) {
				WCHAR name[MAX_PATH];
				size_t j, length = strlen(ordinaryNames[i]);
				TEST_CHECK(length < MAX_PATH);
				for (j = 0; j <= length; ++j)
					name[j] = (WCHAR)(unsigned char)ordinaryNames[i][j];
				handle = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
			} else {
				handle = CreateFileA(ordinaryNames[i], GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
			}
			TEST_CHECK(handle == INVALID_HANDLE_VALUE);
			TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
		}
	}
	SetLastError(seed);
	TEST_CHECK(CreateFileA("absent\\NUL.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL) ==
			   INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(ERROR_PATH_NOT_FOUND, GetLastError());
}

static void checkOpenModes(void) {
	static const DWORD accesses[] = {
		0,
		GENERIC_READ,
		GENERIC_WRITE,
		GENERIC_READ | GENERIC_WRITE,
		GENERIC_EXECUTE,
		GENERIC_ALL,
		FILE_APPEND_DATA,
		FILE_READ_ATTRIBUTES,
		MAXIMUM_ALLOWED,
	};
	static const DWORD attributes[] = {
		FILE_ATTRIBUTE_NORMAL,		FILE_ATTRIBUTE_READONLY,	  FILE_ATTRIBUTE_DIRECTORY, FILE_FLAG_NO_BUFFERING,
		FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT, FILE_FLAG_WRITE_THROUGH,
	};
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	HANDLE handle, first, second, duplicate = NULL;
	DWORD handleFlags;
	unsigned i;
	for (i = CREATE_NEW; i <= TRUNCATE_EXISTING; ++i) {
		handle = openDevice("NUL", GENERIC_READ | GENERIC_WRITE, i, 0, NULL, 0,
							i == OPEN_ALWAYS ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
		TEST_CHECK(CloseHandle(handle));
	}
	for (i = 0; i < sizeof(accesses) / sizeof(accesses[0]); ++i) {
		handle = openDevice("NUL", accesses[i], OPEN_EXISTING, 0, NULL, 0, 0);
		TEST_CHECK(CloseHandle(handle));
	}
	for (i = 0; i < sizeof(attributes) / sizeof(attributes[0]); ++i) {
		handle = openDevice("NUL", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, attributes[i], NULL, 0, 0);
		TEST_CHECK(CloseHandle(handle));
	}
	SetLastError(seed);
	TEST_CHECK(CreateFileA("NUL", GENERIC_READ, 0, NULL, 0, 0, NULL) == INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(seed);
	TEST_CHECK(CreateFileW(L"NUL", GENERIC_READ, 0, NULL, 6, 0, NULL) == INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(seed);
	TEST_CHECK(CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE,
						   NULL) == INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	handle = openDevice("NUL", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0, &security, 0, 0);
	TEST_CHECK(CloseHandle(handle));
	security.bInheritHandle = FALSE;
	handle = openDevice("NUL", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0, &security, 1, 0);
	TEST_CHECK(CloseHandle(handle));
	first = openDevice("NUL", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0, NULL, 0, 0);
	second = openDevice("NUL", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0, NULL, 0, 0);
	TEST_CHECK(CloseHandle(second));
	SetLastError(seed);
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), first, GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK(duplicate != NULL && duplicate != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(first));
	SetLastError(seed);
	TEST_CHECK_EQ(FILE_TYPE_CHAR, GetFileType(duplicate));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK(GetHandleInformation(duplicate, &handleFlags));
	TEST_CHECK_EQ(HANDLE_FLAG_INHERIT, handleFlags);
	TEST_CHECK(CloseHandle(duplicate));
}

static void checkIO(DWORD access, int readable, int writable) {
	HANDLE handle = openDevice("NUL", access, OPEN_EXISTING, 0, NULL, 0, 0);
	unsigned char buffer[8], before[8];
	struct CountOutput count = countOutput();
	LARGE_INTEGER size;
	OVERLAPPED overlapped;
	BOOL result;
	memset(buffer, 0xa5, sizeof(buffer));
	memcpy(before, buffer, sizeof(before));
	SetLastError(seed);
	result = ReadFile(handle, buffer, sizeof(buffer), &count.value, NULL);
	TEST_CHECK_EQ(readable, result != FALSE);
	TEST_CHECK_EQ(readable ? seed : ERROR_ACCESS_DENIED, GetLastError());
	checkCount(&count, 0);
	TEST_CHECK(memcmp(buffer, before, sizeof(buffer)) == 0);
	count = countOutput();
	SetLastError(seed);
	result = WriteFile(handle, buffer, sizeof(buffer), &count.value, NULL);
	TEST_CHECK_EQ(writable, result != FALSE);
	TEST_CHECK_EQ(writable ? seed : ERROR_ACCESS_DENIED, GetLastError());
	checkCount(&count, writable ? sizeof(buffer) : 0);
	TEST_CHECK(memcmp(buffer, before, sizeof(buffer)) == 0);
	if (readable) {
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.Offset = 17;
		count = countOutput();
		SetLastError(seed);
		TEST_CHECK(!ReadFile(handle, buffer, sizeof(buffer), &count.value, &overlapped));
		TEST_CHECK_EQ(ERROR_HANDLE_EOF, GetLastError());
		checkCount(&count, 0);
		TEST_CHECK_U64_EQ(0xc0000011, overlapped.Internal);
		TEST_CHECK_U64_EQ(0, overlapped.InternalHigh);
		TEST_CHECK(memcmp(buffer, before, sizeof(buffer)) == 0);
	}
	if (writable) {
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.Offset = 17;
		count = countOutput();
		SetLastError(seed);
		TEST_CHECK(WriteFile(handle, buffer, sizeof(buffer), &count.value, &overlapped));
		TEST_CHECK_EQ(seed, GetLastError());
		checkCount(&count, sizeof(buffer));
		TEST_CHECK_U64_EQ(0, overlapped.Internal);
		TEST_CHECK_U64_EQ(sizeof(buffer), overlapped.InternalHigh);
	}
	size.QuadPart = 0x5555;
	SetLastError(seed);
	TEST_CHECK(GetFileSizeEx(handle, &size));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_U64_EQ(0, size.QuadPart);
	SetLastError(seed);
	TEST_CHECK(!FlushFileBuffers(handle));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(CloseHandle(handle));
}

static void checkUnsupported(void) {
	HANDLE handle = openDevice("NUL", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0, NULL, 0, 0);
	LARGE_INTEGER distance, position;
	distance.QuadPart = 17;
	position.QuadPart = 0x5555;
	SetLastError(seed);
	TEST_CHECK(!SetFilePointerEx(handle, distance, &position, FILE_BEGIN));
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	TEST_CHECK_U64_EQ(0x5555, position.QuadPart);
	SetLastError(seed);
	TEST_CHECK_EQ(INVALID_SET_FILE_POINTER, SetFilePointer(handle, 0, NULL, FILE_CURRENT));
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	SetLastError(seed);
	TEST_CHECK(!SetEndOfFile(handle));
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	TEST_CHECK(CloseHandle(handle));
	SetLastError(seed);
	TEST_CHECK(CreateFileA("NUL", 0x00800000, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
}

int main(int argc, char **argv) {
	char current[MAX_PATH], temp[MAX_PATH], directory[MAX_PATH];
	DWORD length;
	if (argc == 2 && strcmp(argv[1], "--unsupported") == 0) {
		checkUnsupported();
		puts("null device unsupported scope verified");
		return 0;
	}
	TEST_CHECK_EQ(1, argc);
	length = GetCurrentDirectoryA(sizeof(current), current);
	TEST_CHECK(length > 0 && length < sizeof(current));
	length = GetTempPathA(sizeof(temp), temp);
	TEST_CHECK(length > 0 && length < sizeof(temp));
	TEST_CHECK(GetTempFileNameA(temp, "ndv", 0, directory));
	TEST_CHECK(DeleteFileA(directory));
	TEST_CHECK(CreateDirectoryA(directory, NULL));
	TEST_CHECK(SetCurrentDirectoryA(directory));
	checkAliases();
	checkOpenModes();
	checkIO(GENERIC_READ | GENERIC_WRITE, 1, 1);
	checkIO(GENERIC_READ, 1, 0);
	checkIO(GENERIC_WRITE, 0, 1);
	checkIO(0, 0, 0);
	checkIO(FILE_APPEND_DATA, 0, 1);
	TEST_CHECK(SetCurrentDirectoryA(current));
	TEST_CHECK(RemoveDirectoryA(directory));
	puts("null device semantics verified");
	return 0;
}
