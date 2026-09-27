#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef BOOL(WINAPI *PathIsDirectoryFn)(LPCWSTR);

static size_t wideLength(const WCHAR *text) {
	size_t length = 0;
	while (text[length])
		++length;
	return length;
}

static int append(WCHAR *output, const WCHAR *base, const WCHAR *suffix) {
	size_t baseLength = wideLength(base), suffixLength = wideLength(suffix);
	if (baseLength + suffixLength >= MAX_PATH)
		return 0;
	memcpy(output, base, baseLength * sizeof(WCHAR));
	memcpy(output + baseLength, suffix, (suffixLength + 1) * sizeof(WCHAR));
	return 1;
}

static int hexDigit(char value) {
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static int decodePath(const char *hex, WCHAR *output) {
	size_t length = strlen(hex);
	if (!length || length % 4 || length / 4 >= MAX_PATH)
		return 0;
	for (size_t index = 0; index < length / 4; ++index) {
		int digits[4];
		for (unsigned digit = 0; digit < 4; ++digit) {
			digits[digit] = hexDigit(hex[index * 4 + digit]);
			if (digits[digit] < 0)
				return 0;
		}
		output[index] = (WCHAR)((digits[0] << 4) | digits[1] | (digits[2] << 12) | (digits[3] << 8));
		if (!output[index])
			return 0;
	}
	output[length / 4] = 0;
	return 1;
}

static int checkPath(PathIsDirectoryFn query, const char *name, const WCHAR *path, BOOL expected, DWORD expectedError) {
	struct {
		uint64_t before;
		WCHAR path[MAX_PATH];
		uint64_t after;
	} storage, original;
	memset(&storage, 0xa5, sizeof(storage));
	if (path) {
		size_t length = wideLength(path);
		if (length >= MAX_PATH)
			return 0;
		memcpy(storage.path, path, (length + 1) * sizeof(WCHAR));
	}
	memcpy(&original, &storage, sizeof(storage));
	SetLastError(0x4321);
	BOOL result = query(path ? storage.path : NULL);
	DWORD error = GetLastError();
	printf("case=%s result=%ld error=%lu\n", name, (long)result, (unsigned long)error);
	if (result != expected || error != expectedError || memcmp(&storage, &original, sizeof(storage))) {
		fprintf(stderr, "path query mismatch: %s\n", name);
		return 0;
	}
	return 1;
}

int main(void) {
	int result = 1, temporaryCreated = 0, rootCreated = 0, unicodeCreated = 0, fileCreated = 0;
	HMODULE module = LoadLibraryA("shlwapi.dll");
	HANDLE file = INVALID_HANDLE_VALUE;
	WCHAR temporary[MAX_PATH], root[MAX_PATH] = {0}, filename[MAX_PATH] = {0}, missing[MAX_PATH];
	WCHAR unicode[MAX_PATH] = {0}, trailing[MAX_PATH];
	PathIsDirectoryFn query = NULL;
	if (!module)
		goto cleanup;
	FARPROC address = GetProcAddress(module, "PathIsDirectoryW");
	_Static_assert(sizeof(address) == sizeof(query), "function pointer size");
	memcpy(&query, &address, sizeof(query));
	if (!query)
		goto cleanup;
	DWORD length = GetTempPathW(MAX_PATH, temporary);
	if (!length || length >= MAX_PATH || !GetTempFileNameW(temporary, L"pdi", 0, root))
		goto cleanup;
	temporaryCreated = 1;
	if (!DeleteFileW(root))
		goto cleanup;
	temporaryCreated = 0;
	if (!CreateDirectoryW(root, NULL))
		goto cleanup;
	rootCreated = 1;
	if (!append(filename, root, L"\\file.bin") || !append(missing, root, L"\\missing"))
		goto cleanup;
	file = CreateFileW(filename, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_NEW,
					   FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		goto cleanup;
	fileCreated = 1;
	if (!CloseHandle(file))
		goto cleanup;
	file = INVALID_HANDLE_VALUE;
	const char *external = getenv("WIBO_FIXTURE_PATH_DIRECTORY_UTF16");
	if (external) {
		if (!decodePath(external, unicode))
			goto cleanup;
	} else {
		if (!append(unicode, root, L"\\directory_\x03a9") || !CreateDirectoryW(unicode, NULL))
			goto cleanup;
		unicodeCreated = 1;
	}
	if (!checkPath(query, "null", NULL, FALSE, 0x4321) ||
		!checkPath(query, "empty", L"", FALSE, ERROR_PATH_NOT_FOUND) ||
		!checkPath(query, "directory", root, FILE_ATTRIBUTE_DIRECTORY, 0x4321) ||
		!checkPath(query, "file", filename, FALSE, 0x4321) ||
		!checkPath(query, "missing", missing, FALSE, ERROR_FILE_NOT_FOUND))
		goto cleanup;
	if (!append(trailing, root, L"\\") ||
		!checkPath(query, "directory-trailing", trailing, FILE_ATTRIBUTE_DIRECTORY, 0x4321))
		goto cleanup;
	if (!append(trailing, filename, L"\\") || !checkPath(query, "file-trailing", trailing, FALSE, ERROR_PATH_NOT_FOUND))
		goto cleanup;
	if (!checkPath(query, "unicode-directory", unicode, FILE_ATTRIBUTE_DIRECTORY, 0x4321) ||
		!append(trailing, unicode, L"\\") ||
		!checkPath(query, "unicode-trailing", trailing, FILE_ATTRIBUTE_DIRECTORY, 0x4321))
		goto cleanup;
	result = 0;
cleanup:
	if (result)
		fprintf(stderr, "directory fixture failed: error=%lu\n", (unsigned long)GetLastError());
	if (file != INVALID_HANDLE_VALUE && !CloseHandle(file))
		result = 1;
	if (fileCreated && !DeleteFileW(filename))
		result = 1;
	if (unicodeCreated && !RemoveDirectoryW(unicode))
		result = 1;
	if (rootCreated && !RemoveDirectoryW(root))
		result = 1;
	if (temporaryCreated && !DeleteFileW(root))
		result = 1;
	if (module && !FreeLibrary(module))
		result = 1;
	return result;
}
