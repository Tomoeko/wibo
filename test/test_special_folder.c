#include "test_assert.h"
#include <wchar.h>
#include <windows.h>

#include <shlobj.h>

int main(void) {
	WCHAR path[MAX_PATH];
	TEST_CHECK_EQ(
		S_OK, SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_CURRENT, path));
	size_t length = wcslen(path);
	TEST_CHECK(length > 0 && length < MAX_PATH);
	TEST_CHECK(path[length - 1] != L'\\' && path[length - 1] != L'/');
	TEST_CHECK_EQ(
		S_OK, SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA | CSIDL_FLAG_DONT_VERIFY, NULL, SHGFP_TYPE_DEFAULT, path));
	path[0] = L'x';
	TEST_CHECK(FAILED(SHGetFolderPathW(NULL, 0xFF, NULL, SHGFP_TYPE_CURRENT, path)));
	TEST_CHECK_EQ(0, path[0]);
	return 0;
}
