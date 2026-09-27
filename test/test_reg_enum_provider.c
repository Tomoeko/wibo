#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

static const DWORD kError = 0x13579bdf;
static void failed_query(HKEY key, LSTATUS expected) {
	WCHAR name[64], cls[64];
	memset(name, 0xa5, sizeof(name));
	memset(cls, 0xa5, sizeof(cls));
	WCHAR original[64];
	memcpy(original, name, sizeof(original));
	DWORD n = 64, c = 64;
	FILETIME time = {0x11223344, 0x55667788};
	SetLastError(kError);
	TEST_CHECK_EQ(expected, RegEnumKeyExW(key, 0, name, &n, NULL, cls, &c, &time));
	TEST_CHECK_EQ(kError, GetLastError());
	TEST_CHECK_EQ(64, n);
	TEST_CHECK_EQ(64, c);
	TEST_CHECK_EQ(0, memcmp(name, original, sizeof(name)));
	TEST_CHECK_EQ(0, memcmp(cls, original, sizeof(cls)));
	TEST_CHECK_EQ(0x11223344, time.dwLowDateTime);
	TEST_CHECK_EQ(0x55667788, time.dwHighDateTime);
}
static void check_snapshot(HKEY key) {
	for (DWORD index = 0; index < 2; ++index) {
		WCHAR name[64], cls[64];
		DWORD n = 64, c = 64;
		FILETIME time;
		SetLastError(kError);
		TEST_CHECK_EQ(ERROR_SUCCESS, RegEnumKeyExW(key, index, name, &n, NULL, cls, &c, &time));
		TEST_CHECK_EQ(kError, GetLastError());
		TEST_CHECK_EQ(0, wcscmp(index ? L"NativeTwo" : L"NativeOne", name));
		TEST_CHECK_EQ(0, wcscmp(index ? L"NativeClassB" : L"NativeClassA", cls));
		TEST_CHECK_EQ(index ? 0x23456789 : 0x12345678, time.dwLowDateTime);
		TEST_CHECK_EQ(index ? 0x02345678 : 0x01234567, time.dwHighDateTime);
	}
}
static void check_ansi(HKEY key, const char *mode) {
	int success = strcmp(mode, "ansi-success") == 0;
	for (unsigned attempt = 0; attempt < 2; ++attempt) {
		char name[64], cls[64], original[64];
		memset(name, 0xa5, sizeof(name));
		memcpy(original, name, sizeof(name));
		memset(cls, 0xa5, sizeof(cls));
		DWORD n = 64, c = 64;
		FILETIME time = {0x11223344, 0x55667788};
		SetLastError(kError);
		LSTATUS status = RegEnumKeyExA(key, 0, name, &n, NULL, cls, &c, &time);
		TEST_CHECK_EQ(success							 ? ERROR_SUCCESS
					  : strcmp(mode, "ansi-failed") == 0 ? ERROR_ACCESS_DENIED
														 : ERROR_INVALID_DATA,
					  status);
		TEST_CHECK_EQ(kError, GetLastError());
		if (success) {
			TEST_CHECK_EQ(2, n);
			TEST_CHECK_EQ(1, c);
			TEST_CHECK_EQ('N', name[0]);
			TEST_CHECK_EQ(0xe9, (unsigned char)name[1]);
			TEST_CHECK_EQ(0, name[2]);
			TEST_CHECK_EQ(0x80, (unsigned char)cls[0]);
			TEST_CHECK_EQ(0, cls[1]);
			TEST_CHECK_EQ(0x12345678, time.dwLowDateTime);
			TEST_CHECK_EQ(0x01234567, time.dwHighDateTime);
		} else {
			TEST_CHECK_EQ(64, n);
			TEST_CHECK_EQ(64, c);
			TEST_CHECK_EQ(0, memcmp(original, name, sizeof(name)));
			TEST_CHECK_EQ(0, memcmp(original, cls, sizeof(cls)));
			TEST_CHECK_EQ(0x11223344, time.dwLowDateTime);
			TEST_CHECK_EQ(0x55667788, time.dwHighDateTime);
		}
	}
}

int main(int argc, char **argv) {
	TEST_CHECK_EQ(2, argc);
	const char *mode = argv[1];
	HKEY key = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS, RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegistryEnumerationProvider", 0,
											   KEY_ENUMERATE_SUB_KEYS, &key));
	if (strncmp(mode, "ansi-", 5) == 0) {
		check_ansi(key, mode);
		RegCloseKey(key);
		return 0;
	}
	if (strcmp(mode, "success") && strcmp(mode, "retry")) {
		LSTATUS error = !strcmp(mode, "failed")		   ? ERROR_ACCESS_DENIED
						: !strcmp(mode, "unavailable") ? ERROR_NOT_SUPPORTED
													   : ERROR_INVALID_DATA;
		failed_query(key, error);
		failed_query(key, error);
		RegCloseKey(key);
		return 0;
	}
	if (!strcmp(mode, "retry"))
		failed_query(key, ERROR_INVALID_DATA);
	check_snapshot(key);
	check_snapshot(key);
	HKEY overlay = NULL;
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegCreateKeyExW(key, L"Overlay", 0, L"LocalClass", 0, KEY_ALL_ACCESS, NULL, &overlay, NULL));
	RegCloseKey(overlay);
	unsigned seen = 0;
	for (DWORD index = 0; index < 3; ++index) {
		WCHAR name[64], cls[64];
		DWORD n = 64, c = 64;
		FILETIME time;
		TEST_CHECK_EQ(ERROR_SUCCESS, RegEnumKeyExW(key, index, name, &n, NULL, cls, &c, &time));
		if (wcscmp(name, L"NativeOne") == 0) {
			seen |= 1;
			TEST_CHECK_EQ(0, wcscmp(cls, L"NativeClassA"));
		} else if (wcscmp(name, L"NativeTwo") == 0) {
			seen |= 2;
			TEST_CHECK_EQ(0, wcscmp(cls, L"NativeClassB"));
		} else {
			TEST_CHECK_EQ(0, wcscmp(name, L"Overlay"));
			TEST_CHECK_EQ(0, wcscmp(cls, L"LocalClass"));
			TEST_CHECK(time.dwLowDateTime || time.dwHighDateTime);
			seen |= 4;
		}
	}
	TEST_CHECK_EQ(7, seen);
	WCHAR name[64];
	DWORD n = 64;
	TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, RegEnumKeyExW(key, 3, name, &n, NULL, NULL, NULL, NULL));
	RegCloseKey(key);
	return 0;
}
