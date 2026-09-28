#include "test_assert.h"

#include <string.h>
#include <wchar.h>
#include <windows.h>

typedef BOOL(WINAPI *path_exists_fn)(LPCWSTR);

int main(void) {
	WCHAR temporary[MAX_PATH], directory[MAX_PATH], file[MAX_PATH], result[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH, temporary);
	TEST_CHECK(length && length < MAX_PATH);
	TEST_CHECK(
		swprintf(directory, MAX_PATH, L"%lssearch_path_%lu_%lu", temporary, GetCurrentProcessId(), GetTickCount()) > 0);
	TEST_CHECK(CreateDirectoryW(directory, NULL));
	TEST_CHECK(swprintf(file, MAX_PATH, L"%ls\\caf\u00e9.bin", directory) > 0);
	HANDLE created = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(created != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(created));
	HMODULE pathLibrary = LoadLibraryW(L"shlwapi.dll");
	TEST_CHECK(pathLibrary != NULL);
	FARPROC address = GetProcAddress(pathLibrary, "PathFileExistsW");
	path_exists_fn pathExists = NULL;
	_Static_assert(sizeof(address) == sizeof(pathExists), "function pointer size");
	memcpy(&pathExists, &address, sizeof(pathExists));
	TEST_CHECK(pathExists != NULL);
	TEST_CHECK(pathExists(file));
	TEST_CHECK(pathExists(directory));

	WCHAR *part = NULL;
	length = SearchPathW(directory, L"caf\u00e9", L".bin", MAX_PATH, result, &part);
	TEST_CHECK(length && length < MAX_PATH);
	TEST_CHECK(part && wcscmp(part, L"caf\u00e9.bin") == 0);
	TEST_CHECK(wcscmp(result, file) == 0);
	TEST_CHECK(part == wcsrchr(result, L'\\') + 1);
	HANDLE opened = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(opened != INVALID_HANDLE_VALUE);
	WCHAR finalPath[MAX_PATH];
	DWORD finalLength = GetFinalPathNameByHandleW(opened, finalPath, MAX_PATH, 0);
	TEST_CHECK(finalLength && finalLength < MAX_PATH);
	TEST_CHECK(wcsncmp(finalPath, L"\\\\?\\", 4) == 0);
	TEST_CHECK(wcscmp(wcsrchr(finalPath, L'\\') + 1, L"caf\u00e9.bin") == 0);
	TEST_CHECK(GetFinalPathNameByHandleW(opened, NULL, 0, 0) == finalLength + 1);
	finalLength = GetFinalPathNameByHandleW(opened, finalPath, MAX_PATH, VOLUME_NAME_NONE);
	TEST_CHECK(finalLength && finalLength < MAX_PATH && finalPath[0] == L'\\');
	TEST_CHECK(wcscmp(wcsrchr(finalPath, L'\\') + 1, L"caf\u00e9.bin") == 0);
	TEST_CHECK(CloseHandle(opened));

	WCHAR tiny[2] = {L'X', L'Y'};
	part = (WCHAR *)1;
	DWORD required = SearchPathW(directory, L"caf\u00e9.bin", L".wrong", 2, tiny, &part);
	TEST_CHECK(required == length + 1);
	TEST_CHECK(tiny[0] == L'X' && tiny[1] == L'Y');

	part = (WCHAR *)1;
	TEST_CHECK(SearchPathW(directory, L"missing", L".bin", MAX_PATH, result, &part) == 0);
	WCHAR missing[MAX_PATH];
	TEST_CHECK(swprintf(missing, MAX_PATH, L"%ls\\missing.bin", directory) > 0);
	TEST_CHECK(!pathExists(missing));
	TEST_CHECK(FreeLibrary(pathLibrary));
	TEST_CHECK(DeleteFileW(file));
	TEST_CHECK(RemoveDirectoryW(directory));
	return 0;
}
