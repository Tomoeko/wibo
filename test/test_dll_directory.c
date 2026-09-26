#include "test_assert.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static void copy_fixture(const char *source, const char *destination) {
	HANDLE input = CreateFileA(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	TEST_CHECK(input != INVALID_HANDLE_VALUE);
	HANDLE output = CreateFileA(destination, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	TEST_CHECK(output != INVALID_HANDLE_VALUE);
	BYTE bytes[4096];
	DWORD count;
	for (;;) {
		TEST_CHECK(ReadFile(input, bytes, sizeof(bytes), &count, NULL));
		if (!count)
			break;
		DWORD written;
		TEST_CHECK(WriteFile(output, bytes, count, &written, NULL));
		TEST_CHECK_EQ(count, written);
	}
	TEST_CHECK(CloseHandle(input));
	TEST_CHECK(CloseHandle(output));
}

static void check_load(const char *name) {
	HMODULE module = LoadLibraryA(name);
	TEST_CHECK(module != NULL);
	TEST_CHECK(FreeLibrary(module));
}

int main(void) {
	char original[MAX_PATH], directory[MAX_PATH], source[MAX_PATH], destination[MAX_PATH], override_path[MAX_PATH];
	WCHAR wide[MAX_PATH];
	TEST_CHECK(GetCurrentDirectoryA(MAX_PATH, original) > 0);
	DWORD prefix = GetTempPathA(MAX_PATH, directory);
	TEST_CHECK(prefix > 0 && prefix < MAX_PATH);
	int suffix = snprintf(directory + prefix, MAX_PATH - prefix, "wibo_directory_%lx_%lx", GetCurrentProcessId(),
						  GetTickCount());
	TEST_CHECK(suffix > 0 && (DWORD)suffix < MAX_PATH - prefix);
	TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, MAX_PATH) > 0);
	TEST_CHECK(CreateDirectoryW(wide, NULL));
	TEST_CHECK(!CreateDirectoryW(wide, NULL));
	TEST_CHECK_EQ(ERROR_ALREADY_EXISTS, GetLastError());
	TEST_CHECK(GetModuleFileNameA(NULL, source, MAX_PATH) > 0);
	char *separator = strrchr(source, '\\');
	TEST_CHECK(separator != NULL);
	TEST_CHECK((size_t)(separator - source) + sizeof("external_exports.dll") < sizeof(source));
	strcpy(separator + 1, "external_exports.dll");
	TEST_CHECK(snprintf(destination, MAX_PATH, "%s\\directory_probe.dll", directory) < MAX_PATH);
	copy_fixture(source, destination);
	TEST_CHECK(snprintf(override_path, MAX_PATH, "%s\\override_probe.dll", directory) < MAX_PATH);
	copy_fixture(source, override_path);
	TEST_CHECK(SetCurrentDirectoryA(directory));
	TEST_CHECK(SetDllDirectoryW(NULL));
	// Some Wine versions retain the current directory after an empty override.
	TEST_CHECK(SetDllDirectoryW(L""));
	TEST_CHECK(LoadLibraryA("directory_probe.dll") == NULL);
	TEST_CHECK_EQ(ERROR_MOD_NOT_FOUND, GetLastError());
	TEST_CHECK(SetDllDirectoryA(NULL));
	check_load("directory_probe.dll");
	TEST_CHECK(SetCurrentDirectoryA(original));
	TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, MAX_PATH) > 0);
	TEST_CHECK(SetDllDirectoryW(wide));
	check_load("override_probe.dll");
	TEST_CHECK(SetDllDirectoryW(NULL));
	TEST_CHECK(DeleteFileA(destination));
	TEST_CHECK(DeleteFileA(override_path));
	TEST_CHECK(RemoveDirectoryA(directory));
	return 0;
}
