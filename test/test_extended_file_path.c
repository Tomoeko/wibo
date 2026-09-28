#include "test_assert.h"

#include <wchar.h>
#include <windows.h>

int main(void) {
	WCHAR path[512] = L"\\\\?\\C:\\";
	const WCHAR segment[] = L"directory-that-does-not-exist-0123456789-abcdefghijklmnop";
	for (unsigned index = 0; index < 5; ++index) {
		wcscat(path, segment);
		wcscat(path, L"\\");
	}
	wcscat(path, L"sample.dat");
	TEST_CHECK(wcslen(path) > 260);

	SetLastError(0);
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file == INVALID_HANDLE_VALUE);
	DWORD error = GetLastError();
	TEST_CHECK(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
	return 0;
}
