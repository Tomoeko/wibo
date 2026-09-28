#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

int main(void) {
	WCHAR path[96];
	TEST_CHECK(swprintf(path, sizeof(path) / sizeof(*path), L"Software\\WiboRegistryMutation_%08lx_%08lx",
						GetCurrentProcessId(), GetTickCount()) > 0);
	HKEY root = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, L"RootClass", 0, KEY_ALL_ACCESS, NULL, &root, NULL));
	HKEY child = NULL, grandchild = NULL, second = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegCreateKeyExW(root, L"ChildName", 0, L"ChildClass", 0, KEY_ALL_ACCESS, NULL, &child, NULL));
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegCreateKeyExW(child, L"Nested", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &grandchild, NULL));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCreateKeyExW(root, L"Other", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &second, NULL));
	const DWORD number = 0x12345678;
	const BYTE bytes[] = {1, 2, 3, 4, 5, 6};
	TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(root, L"Number", 0, REG_DWORD, (const BYTE *)&number, sizeof(number)));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(root, L"LongerName", 0, REG_BINARY, bytes, sizeof(bytes)));
	WCHAR keyClass[32];
	DWORD classLength = 32, subkeys = 0, longestSubkey = 0, longestSubkeyClass = 0;
	DWORD values = 0, longestValueName = 0, longestValueData = 0;
	FILETIME written = {0, 0};
	const DWORD savedError = 0x13579bdf;
	SetLastError(savedError);
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegQueryInfoKeyW(root, keyClass, &classLength, NULL, &subkeys, &longestSubkey, &longestSubkeyClass,
								   &values, &longestValueName, &longestValueData, NULL, &written));
	TEST_CHECK_EQ(savedError, GetLastError());
	TEST_CHECK_EQ(0, wcscmp(keyClass, L"RootClass"));
	TEST_CHECK_EQ(9, classLength);
	TEST_CHECK_EQ(2, subkeys);
	TEST_CHECK_EQ(9, longestSubkey);
	TEST_CHECK_EQ(10, longestSubkeyClass);
	TEST_CHECK_EQ(2, values);
	TEST_CHECK_EQ(10, longestValueName);
	TEST_CHECK_EQ(6, longestValueData);
	TEST_CHECK(written.dwLowDateTime || written.dwHighDateTime);
	char ansiClass[32];
	classLength = sizeof(ansiClass);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegQueryInfoKeyA(root, ansiClass, &classLength, NULL, NULL, NULL, NULL, NULL, NULL,
												  NULL, NULL, NULL));
	TEST_CHECK_STR_EQ("RootClass", ansiClass);
	TEST_CHECK_EQ(9, classLength);
	classLength = 2;
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER,
				  RegQueryInfoKeyW(root, keyClass, &classLength, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, RegDeleteKeyW(root, L"ChildName"));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegDeleteKeyW(child, L"Nested"));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegDeleteKeyA(root, "childname"));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegDeleteKeyW(root, L"Other"));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, RegDeleteKeyW(root, L"Other"));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegDeleteValueA(root, "NUMBER"));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegDeleteValueW(root, L"longername"));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, RegDeleteValueW(root, L"longername"));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, RegQueryValueExW(root, L"Number", NULL, NULL, NULL, NULL));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(second));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(grandchild));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(child));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(root));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegDeleteKeyW(HKEY_CURRENT_USER, path));
	return 0;
}
