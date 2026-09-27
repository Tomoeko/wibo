#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef BOOL(WINAPI *PathIsDirectoryFn)(LPCWSTR);

struct GuardedAttributes {
	uint64_t before;
	WIN32_FILE_ATTRIBUTE_DATA value;
	uint64_t after;
};

static const uint64_t guard = UINT64_C(0x194b3c7d528ea601);

static int queryAttributes(const void *path, BOOL wide, struct GuardedAttributes *output, BOOL expected,
						   DWORD expectedError) {
	memset(output, 0xa5, sizeof(*output));
	output->before = output->after = guard;
	SetLastError(0x4321);
	BOOL result = wide ? GetFileAttributesExW(path, GetFileExInfoStandard, &output->value)
					   : GetFileAttributesExA(path, GetFileExInfoStandard, &output->value);
	DWORD error = GetLastError();
	printf("extended wide=%ld result=%ld error=%lu attributes=%lu size=%lu:%lu\n", (long)wide, (long)result,
		   (unsigned long)error, (unsigned long)output->value.dwFileAttributes,
		   (unsigned long)output->value.nFileSizeHigh, (unsigned long)output->value.nFileSizeLow);
	return !!result == !!expected && (result || error == expectedError) && output->before == guard &&
		   output->after == guard;
}

static int appendWide(WCHAR *output, const WCHAR *base, const WCHAR *suffix) {
	size_t baseLength = 0, suffixLength = 0;
	while (base[baseLength])
		++baseLength;
	while (suffix[suffixLength])
		++suffixLength;
	if (baseLength + suffixLength >= MAX_PATH)
		return 0;
	memcpy(output, base, baseLength * sizeof(WCHAR));
	memcpy(output + baseLength, suffix, (suffixLength + 1) * sizeof(WCHAR));
	return 1;
}

#define CHECK(condition)                                                                                               \
	do {                                                                                                               \
		if (!(condition)) {                                                                                            \
			fprintf(stderr, "attribute fixture failed at line %d: error=%lu\n", __LINE__,                              \
					(unsigned long)GetLastError());                                                                    \
			goto cleanup;                                                                                              \
		}                                                                                                              \
	} while (0)

int main(void) {
	static const char payload[] = "owned filesystem attribute fixture payload";
	int result = 1, temporaryCreated = 0, rootCreated = 0, directoryCreated = 0, fileCreated = 0, unicodeCreated = 0;
	HANDLE file = INVALID_HANDLE_VALUE;
	HMODULE module = NULL;
	PathIsDirectoryFn pathIsDirectory = NULL;
	char temporary[MAX_PATH], root[MAX_PATH] = {0}, path[MAX_PATH] = {0};
	WCHAR widePath[MAX_PATH], wideRoot[MAX_PATH], unicodePath[MAX_PATH];
	struct GuardedAttributes extended;
	BY_HANDLE_FILE_INFORMATION receipt;
	DWORD written = 0;

	module = LoadLibraryA("shlwapi.dll");
	CHECK(module);
	FARPROC address = GetProcAddress(module, "PathIsDirectoryW");
	_Static_assert(sizeof(address) == sizeof(pathIsDirectory), "function pointer size");
	memcpy(&pathIsDirectory, &address, sizeof(pathIsDirectory));
	CHECK(pathIsDirectory);
	DWORD length = GetTempPathA(MAX_PATH, temporary);
	CHECK(length && length < MAX_PATH);
	CHECK(GetTempFileNameA(temporary, "fai", 0, root));
	temporaryCreated = 1;
	CHECK(DeleteFileA(root));
	temporaryCreated = 0;
	CHECK(CreateDirectoryA(root, NULL));
	rootCreated = 1;
	int pathLength = snprintf(path, sizeof(path), "%s\\license.dat", root);
	CHECK(pathLength > 0 && (size_t)pathLength < sizeof(path));
	CHECK(MultiByteToWideChar(CP_ACP, 0, path, -1, widePath, MAX_PATH));
	CHECK(MultiByteToWideChar(CP_ACP, 0, root, -1, wideRoot, MAX_PATH));

	SetLastError(0x4321);
	DWORD attributes = GetFileAttributesA(path);
	DWORD error = GetLastError();
	printf("missing attributes=%lu error=%lu\n", (unsigned long)attributes, (unsigned long)error);
	CHECK(attributes == INVALID_FILE_ATTRIBUTES && error == ERROR_FILE_NOT_FOUND);
	CHECK(queryAttributes(path, FALSE, &extended, FALSE, ERROR_FILE_NOT_FOUND));
	CHECK(GetFileAttributesW(widePath) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);
	CHECK(queryAttributes(widePath, TRUE, &extended, FALSE, ERROR_FILE_NOT_FOUND));
	CHECK(!pathIsDirectory(widePath));

	CHECK(CreateDirectoryA(path, NULL));
	directoryCreated = 1;
	attributes = GetFileAttributesA(path);
	CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY));
	CHECK(queryAttributes(path, FALSE, &extended, TRUE, 0));
	CHECK(extended.value.dwFileAttributes == attributes &&
		  (extended.value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
	CHECK(GetFileAttributesW(widePath) == attributes);
	CHECK(queryAttributes(widePath, TRUE, &extended, TRUE, 0));
	CHECK(extended.value.dwFileAttributes == attributes);
	CHECK(pathIsDirectory(widePath));
	CHECK(RemoveDirectoryA(path));
	directoryCreated = 0;

	file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
					   NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	fileCreated = 1;
	CHECK(WriteFile(file, payload, sizeof(payload) - 1, &written, NULL));
	CHECK(written == sizeof(payload) - 1);
	CHECK(CloseHandle(file));
	file = INVALID_HANDLE_VALUE;
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
					   FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	CHECK(GetFileInformationByHandle(file, &receipt));
	attributes = GetFileAttributesA(path);
	CHECK(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY));
	CHECK(queryAttributes(path, FALSE, &extended, TRUE, 0));
	CHECK(attributes == receipt.dwFileAttributes && extended.value.dwFileAttributes == receipt.dwFileAttributes);
	CHECK(receipt.nFileSizeHigh == 0 && receipt.nFileSizeLow == sizeof(payload) - 1);
	CHECK(extended.value.nFileSizeHigh == receipt.nFileSizeHigh && extended.value.nFileSizeLow == receipt.nFileSizeLow);
	CHECK(!memcmp(&extended.value.ftCreationTime, &receipt.ftCreationTime, sizeof(FILETIME)));
	CHECK(!memcmp(&extended.value.ftLastAccessTime, &receipt.ftLastAccessTime, sizeof(FILETIME)));
	CHECK(!memcmp(&extended.value.ftLastWriteTime, &receipt.ftLastWriteTime, sizeof(FILETIME)));
	CHECK(GetFileAttributesW(widePath) == attributes);
	CHECK(queryAttributes(widePath, TRUE, &extended, TRUE, 0));
	CHECK(extended.value.dwFileAttributes == receipt.dwFileAttributes && extended.value.nFileSizeHigh == 0 &&
		  extended.value.nFileSizeLow == receipt.nFileSizeLow);
	CHECK(!memcmp(&extended.value.ftCreationTime, &receipt.ftCreationTime, sizeof(FILETIME)));
	CHECK(!memcmp(&extended.value.ftLastAccessTime, &receipt.ftLastAccessTime, sizeof(FILETIME)));
	CHECK(!memcmp(&extended.value.ftLastWriteTime, &receipt.ftLastWriteTime, sizeof(FILETIME)));
	CHECK(!pathIsDirectory(widePath));
	printf("file attributes=%lu size=%lu creation=%lu:%lu write=%lu:%lu\n", (unsigned long)attributes,
		   (unsigned long)receipt.nFileSizeLow, (unsigned long)receipt.ftCreationTime.dwHighDateTime,
		   (unsigned long)receipt.ftCreationTime.dwLowDateTime, (unsigned long)receipt.ftLastWriteTime.dwHighDateTime,
		   (unsigned long)receipt.ftLastWriteTime.dwLowDateTime);

	CHECK(appendWide(unicodePath, wideRoot, L"\\directory_\x03a9_\xd83d\xde00"));
	CHECK(GetFileAttributesW(unicodePath) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);
	CHECK(queryAttributes(unicodePath, TRUE, &extended, FALSE, ERROR_FILE_NOT_FOUND));
	CHECK(CreateDirectoryW(unicodePath, NULL));
	unicodeCreated = 1;
	attributes = GetFileAttributesW(unicodePath);
	CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY));
	CHECK(queryAttributes(unicodePath, TRUE, &extended, TRUE, 0));
	CHECK(extended.value.dwFileAttributes == attributes);
	CHECK(pathIsDirectory(unicodePath));
	result = 0;

cleanup:
	if (file != INVALID_HANDLE_VALUE && !CloseHandle(file))
		result = 1;
	if (fileCreated && !DeleteFileA(path))
		result = 1;
	if (directoryCreated && !RemoveDirectoryA(path))
		result = 1;
	if (unicodeCreated && !RemoveDirectoryW(unicodePath))
		result = 1;
	if (rootCreated && !RemoveDirectoryA(root))
		result = 1;
	if (temporaryCreated && !DeleteFileA(root))
		result = 1;
	if (module && !FreeLibrary(module))
		result = 1;
	return result;
}
