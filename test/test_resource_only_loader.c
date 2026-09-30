#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

static void check_resource(HMODULE module) {
	HRSRC resource = FindResourceA(module, MAKEINTRESOURCEA(101), RT_RCDATA);
	TEST_CHECK(resource != NULL);
	TEST_CHECK_EQ(16, SizeofResource(module, resource));
	HGLOBAL loaded = LoadResource(module, resource);
	TEST_CHECK(loaded != NULL);
	const unsigned char *bytes = (const unsigned char *)LockResource(loaded);
	TEST_CHECK(bytes != NULL);
	for (unsigned index = 0; index < 16; ++index)
		TEST_CHECK_EQ(index + 1, bytes[index]);
}

static void check_write_excluded(const char *path) {
	HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file == INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(ERROR_SHARING_VIOLATION, GetLastError());
}

int main(void) {
	char fixturePath[MAX_PATH];
	DWORD pathLength = GetModuleFileNameA(NULL, fixturePath, sizeof(fixturePath));
	TEST_CHECK(pathLength > 0 && pathLength < sizeof(fixturePath));
	char *baseName = strrchr(fixturePath, '\\');
	if (!baseName)
		baseName = strrchr(fixturePath, '/');
	TEST_CHECK(baseName != NULL);
	const char suffix[] = "resource_only_fixture.dll";
	TEST_CHECK((size_t)(baseName - fixturePath) + sizeof(suffix) < sizeof(fixturePath));
	memcpy(baseName + 1, suffix, sizeof(suffix));
	const DWORD flags = LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE;
	HMODULE first = LoadLibraryExW(L"resource_only_fixture.dll", NULL, flags);
	TEST_CHECK_MSG(first != NULL, "resource image load failed: %lu", (unsigned long)GetLastError());
	HMODULE second = LoadLibraryExA("resource_only_fixture.dll", NULL, flags);
	TEST_CHECK(second != NULL);
	TEST_CHECK(first != second);
	TEST_CHECK(((uintptr_t)first & 3) != 0);
	TEST_CHECK(((uintptr_t)second & 3) != 0);
	TEST_CHECK(GetModuleHandleA("resource_only_fixture.dll") == NULL);
	TEST_CHECK(GetProcAddress(first, "fixture_marker") == NULL);
	check_resource(first);
	check_resource(second);
	check_write_excluded(fixturePath);
	TEST_CHECK(FreeLibrary(first));
	TEST_CHECK(FreeLibrary(second));
	HANDLE writable = CreateFileA(fixturePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
								  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(writable != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(writable));

	HMODULE image = LoadLibraryExW(L"resource_only_fixture.dll", NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	TEST_CHECK(image != NULL);
	check_resource(image);
	check_write_excluded(fixturePath);
	TEST_CHECK(FreeLibrary(image));
	const DWORD dataModes[] = {LOAD_LIBRARY_AS_DATAFILE, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE};
	for (unsigned index = 0; index < sizeof(dataModes) / sizeof(dataModes[0]); ++index) {
		HMODULE data = LoadLibraryExW(L"resource_only_fixture.dll", NULL, dataModes[index]);
		TEST_CHECK(data != NULL);
		TEST_CHECK(((uintptr_t)data & 1) != 0);
		const unsigned char *raw = (const unsigned char *)((uintptr_t)data & ~(uintptr_t)3);
		TEST_CHECK_EQ('M', raw[0]);
		TEST_CHECK_EQ('Z', raw[1]);
		check_resource(data);
		if (dataModes[index] == LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)
			check_write_excluded(fixturePath);
		TEST_CHECK(FreeLibrary(data));
	}
	TEST_CHECK(LoadLibraryA("resource_only_fixture.dll") == NULL);
	return 0;
}
