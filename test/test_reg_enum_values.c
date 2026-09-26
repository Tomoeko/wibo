#include <windows.h>

#include "test_assert.h"

static const DWORD sentinel = 0x20001234;

static LONG enumerate(HKEY key, DWORD index, WCHAR *name, DWORD *length, DWORD *type, BYTE *data, DWORD *bytes) {
	SetLastError(sentinel);
	LONG status = RegEnumValueW(key, index, name, length, NULL, type, data, bytes);
	TEST_CHECK_EQ(sentinel, GetLastError());
	return status;
}

int main(void) {
	char path[96];
	snprintf(path, sizeof(path), "WiboRegEnum_%08lx_%08lx", GetCurrentProcessId(), GetTickCount());
	HKEY key;
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL));
	printf("owned_key=%s\n", path);
	fflush(stdout);
	WCHAR name[40];
	DWORD length = 40, size = 32, type;
	BYTE data[32];
	TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, enumerate(key, 0, name, &length, &type, data, &size));
	DWORD number = 99;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(key, L"MiXeD", 0, REG_DWORD, (BYTE *)&number, sizeof(number)));
	number = 100;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(key, L"MIXED", 0, REG_DWORD, (BYTE *)&number, sizeof(number)));
	const WCHAR unicode[] = {0x4e2d, 0xd83d, 0xde00, 0};
	const WCHAR text[] = {'a', 'b', 'c', 0xffff};
	const DWORD textBytes = 3 * sizeof(WCHAR);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(key, unicode, 0, REG_SZ, (BYTE *)text, textBytes));
	const BYTE binary[] = {0, 0xff, 0x17};
	TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(key, NULL, 0, REG_BINARY, binary, sizeof(binary)));
	unsigned seen = 0;
	for (DWORD index = 0; index < 3; ++index) {
		length = 40;
		size = sizeof(data);
		TEST_CHECK_EQ(ERROR_SUCCESS, enumerate(key, index, name, &length, &type, data, &size));
		TEST_CHECK_EQ(wcslen(name), length);
		unsigned bit;
		if (!name[0]) {
			bit = 1;
			TEST_CHECK_EQ(REG_BINARY, type);
			TEST_CHECK_EQ(sizeof(binary), size);
			TEST_CHECK(memcmp(data, binary, size) == 0);
		} else if (wcscmp(name, L"MiXeD") == 0) {
			bit = 2;
			TEST_CHECK_EQ(REG_DWORD, type);
			TEST_CHECK_EQ(sizeof(number), size);
			TEST_CHECK(memcmp(data, &number, size) == 0);
		} else {
			bit = 4;
			TEST_CHECK(wcscmp(name, unicode) == 0);
			TEST_CHECK_EQ(REG_SZ, type);
			TEST_CHECK_EQ(textBytes, size);
			TEST_CHECK(memcmp(data, text, size) == 0);
		}
		TEST_CHECK(!(seen & bit));
		seen |= bit;
		DWORD required = size;
		length = 40;
		size = 0;
		TEST_CHECK_EQ(ERROR_SUCCESS, enumerate(key, index, name, &length, NULL, NULL, &size));
		TEST_CHECK_EQ(required, size);
		DWORD nameLength = length;
		WCHAR guarded[42];
		for (unsigned i = 0; i < 42; ++i)
			guarded[i] = 0xcccc;
		length = nameLength;
		size = sizeof(data);
		TEST_CHECK_EQ(ERROR_MORE_DATA, enumerate(key, index, guarded + 1, &length, &type, data, &size));
		TEST_CHECK_EQ(nameLength, length);
		TEST_CHECK_EQ(0xcccc, guarded[0]);
		TEST_CHECK_EQ(0xcccc, guarded[41]);
		length = 40;
		size = required - 1;
		TEST_CHECK_EQ(ERROR_MORE_DATA, enumerate(key, index, name, &length, &type, data, &size));
		TEST_CHECK_EQ(required, size);
	}
	TEST_CHECK_EQ(7, seen);
	length = 40;
	TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, enumerate(key, 3, name, &length, NULL, NULL, NULL));
	length = 40;
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, enumerate((HKEY)(ULONG_PTR)0x123456, 0, name, &length, NULL, NULL, NULL));
	length = 40;
	SetLastError(sentinel);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, RegEnumValueW(key, 0, NULL, &length, NULL, NULL, NULL, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, RegEnumValueW(key, 0, name, NULL, NULL, NULL, NULL, NULL));
	DWORD reserved = 0;
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, RegEnumValueW(key, 0, name, &length, &reserved, NULL, NULL, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, RegEnumValueW(key, 0, name, &length, NULL, NULL, data, NULL));
	TEST_CHECK_EQ(sentinel, GetLastError());
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(key));
	return 0;
}
