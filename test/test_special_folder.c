#include "test_assert.h"
#include <wchar.h>
#include <windows.h>

#include <shlobj.h>

static const GUID common = {0x62ab5d82, 0xfdc1, 0x4dc3, {0xa9, 0xdd, 7, 0x0d, 0x1d, 0x49, 0x5d, 0x97}};
static const GUID profile = {0x5e6c858f, 0x0e22, 0x4760, {0x9a, 0xfe, 0xea, 0x33, 0x17, 0xb6, 0x71, 0x73}};
static const GUID missing = {0};

static void known_folder(const GUID *id, DWORD flags) {
	WCHAR *first = NULL, *second = NULL;
	SetLastError(0x1234);
	TEST_CHECK_EQ(S_OK, SHGetKnownFolderPath(id, flags, NULL, &first));
	TEST_CHECK_EQ(0x1234, GetLastError());
	TEST_CHECK(first != NULL && wcslen(first) > 0);
	const size_t length = wcslen(first);
	if (getenv("WIBO_FIXTURE_PROVIDER") && id == &profile)
		TEST_CHECK(wcscmp(first, L"C:\\Fixture\\profile-\u4e2d-\U0001f600") == 0);

	TEST_CHECK(first[length - 1] != L'\\' && first[length - 1] != L'/');
	TEST_CHECK_EQ(S_OK, SHGetKnownFolderPath(id, flags, NULL, &second));
	TEST_CHECK(first != second);
	TEST_CHECK(wcscmp(first, second) == 0);
	CoTaskMemFree(first);
	CoTaskMemFree(second);
}

int main(void) {
	if (getenv("WIBO_FIXTURE_FOLDER_RESPONSE")) {
		WCHAR *output = (WCHAR *)(uintptr_t)1;
		TEST_CHECK_EQ(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), SHGetKnownFolderPath(&common, 0x4000, NULL, &output));
		TEST_CHECK(output == NULL);
		return EXIT_SUCCESS;
	}
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
	known_folder(&common, 0x4000);
	known_folder(&common, 0x4400);
	known_folder(&profile, 0x4000);
	WCHAR *output = (WCHAR *)(uintptr_t)1;
	const HRESULT status = SHGetKnownFolderPath(&missing, 0, NULL, &output);
	TEST_CHECK_EQ(E_INVALIDARG, status);
	TEST_CHECK(output == NULL);
	CoTaskMemFree(output);
	return 0;
}
