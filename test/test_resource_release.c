#include <windows.h>

#include "test_assert.h"

// Modern FreeResource preserves resources mapped with their owning module.
// https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freeresource
int main(void) {
	static const unsigned char expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	HMODULE module = GetModuleHandleA(NULL);
	TEST_CHECK(module != NULL);
	HRSRC resource = FindResourceA(module, MAKEINTRESOURCEA(101), RT_RCDATA);
	TEST_CHECK(resource != NULL);
	TEST_CHECK_EQ(sizeof(expected), SizeofResource(module, resource));
	HGLOBAL loaded = LoadResource(module, resource);
	TEST_CHECK(loaded != NULL);
	const void *bytes = LockResource(loaded);
	TEST_CHECK(bytes != NULL);
	TEST_CHECK_EQ(0, memcmp(expected, bytes, sizeof(expected)));
	TEST_CHECK(loaded == LoadResource(module, resource));
	for (unsigned int iteration = 0; iteration < 3; ++iteration) {
		const DWORD sentinel = 0x13572468;
		SetLastError(sentinel);
		TEST_CHECK_EQ(FALSE, FreeResource(loaded));
		TEST_CHECK_EQ(sentinel, GetLastError());
		TEST_CHECK(bytes == LockResource(loaded));
		TEST_CHECK_EQ(0, memcmp(expected, bytes, sizeof(expected)));
		TEST_CHECK(loaded == LoadResource(module, resource));
		TEST_CHECK_EQ(sizeof(expected), SizeofResource(module, resource));
		TEST_CHECK(resource == FindResourceA(module, MAKEINTRESOURCEA(101), RT_RCDATA));
	}
	puts("resource release tests passed: repeated releases preserve owned RCDATA bytes and identity");
	return 0;
}
