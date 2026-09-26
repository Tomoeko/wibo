#include "test_assert.h"
#include <windows.h>

int main(void) {
	HKEY key = NULL;
	const WCHAR path[] = L"Software\\WiboProviderFixture";
	const WCHAR text[] = {0x4E2D, 0, 0xD83D, 0xDE00, 0};
	const DWORD number = 0xfedcba98;
	const BYTE binary[] = {0, 0xff, 0x17};
	if (!getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(ERROR_SUCCESS,
					  RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL));
		TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(key, L"Text", 0, REG_SZ, (const BYTE *)text, sizeof(text)));
		TEST_CHECK_EQ(ERROR_SUCCESS,
					  RegSetValueExW(key, L"Number", 0, REG_DWORD, (const BYTE *)&number, sizeof(number)));
		TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(key, NULL, 0, REG_BINARY, binary, sizeof(binary)));
		RegCloseKey(key);
	}
	TEST_CHECK_EQ(ERROR_SUCCESS, RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key));
	DWORD type = 0, size = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegQueryValueExW(key, L"Text", NULL, &type, NULL, &size));
	TEST_CHECK_EQ(REG_SZ, type);
	TEST_CHECK_EQ(sizeof(text), size);
	WCHAR actual[5];
	size = 2;
	TEST_CHECK_EQ(ERROR_MORE_DATA, RegQueryValueExW(key, L"Text", NULL, &type, (BYTE *)actual, &size));
	TEST_CHECK_EQ(sizeof(text), size);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegQueryValueExW(key, L"Text", NULL, &type, (BYTE *)actual, &size));
	TEST_CHECK(memcmp(actual, text, sizeof(text)) == 0);
	DWORD actualNumber = 0;
	size = sizeof(actualNumber);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegQueryValueExW(key, L"Number", NULL, &type, (BYTE *)&actualNumber, &size));
	TEST_CHECK_EQ(REG_DWORD, type);
	TEST_CHECK_U64_EQ(number, actualNumber);
	BYTE actualBinary[3];
	size = sizeof(actualBinary);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegQueryValueExW(key, NULL, NULL, &type, actualBinary, &size));
	TEST_CHECK_EQ(REG_BINARY, type);
	TEST_CHECK(memcmp(actualBinary, binary, sizeof(binary)) == 0);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, RegQueryValueExW(key, L"Missing", NULL, &type, NULL, &size));
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		size = sizeof(actualNumber);
		TEST_CHECK_EQ(ERROR_SUCCESS, RegQueryValueExW(key, L"View", NULL, &type, (BYTE *)&actualNumber, &size));
		TEST_CHECK_EQ(64, actualNumber);
		HKEY other = NULL;
		TEST_CHECK_EQ(ERROR_SUCCESS,
					  RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &other));
		TEST_CHECK_EQ(ERROR_SUCCESS, RegQueryValueExW(other, L"View", NULL, &type, (BYTE *)&actualNumber, &size));
		TEST_CHECK_EQ(32, actualNumber);
		RegCloseKey(other);
		const DWORD changed = 17;
		TEST_CHECK_EQ(ERROR_SUCCESS,
					  RegSetValueExW(key, L"Number", 0, REG_DWORD, (const BYTE *)&changed, sizeof(changed)));
		TEST_CHECK_EQ(ERROR_SUCCESS, RegQueryValueExW(key, L"Number", NULL, &type, (BYTE *)&actualNumber, &size));
		TEST_CHECK_EQ(17, actualNumber);
	}
	RegCloseKey(key);
	if (!getenv("WIBO_FIXTURE_PROVIDER"))
		RegDeleteKeyW(HKEY_CURRENT_USER, path);
	return 0;
}
