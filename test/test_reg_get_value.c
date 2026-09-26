#include "test_assert.h"
#include <windows.h>

static HKEY key;
static void put(const WCHAR *name, DWORD type, const void *data, DWORD size) {
	TEST_CHECK_EQ(ERROR_SUCCESS, RegSetValueExW(key, name, 0, type, data, size));
}
static LONG get(const WCHAR *name, DWORD flags, DWORD *type, void *data, DWORD *size) {
	SetLastError(0x71);
	LONG status = RegGetValueW(key, NULL, name, flags, type, data, size);
	TEST_CHECK_EQ(0x71, GetLastError());
	return status;
}
int main(void) {
	WCHAR path[80];
	swprintf(path, 80, L"WiboRegGet_%08lx_%08lx", GetCurrentProcessId(), GetTickCount());
	printf("owned_key=%ls\n", path);
	fflush(stdout);
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL));
	const DWORD number = 0x12345678;
	put(L"Number", REG_DWORD, &number, sizeof(number));
	DWORD type = 0, size = 0, output = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"number", RRF_RT_REG_DWORD, &type, NULL, &size));
	TEST_CHECK_EQ(REG_DWORD, type);
	TEST_CHECK_EQ(sizeof(number), size);
	size = sizeof(output);
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"NUMBER", RRF_RT_DWORD, &type, &output, &size));
	TEST_CHECK_EQ(number, output);
	size = sizeof(output);
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegGetValueW(HKEY_CURRENT_USER, path, L"Number", RRF_RT_REG_DWORD, &type, &output, &size));
	TEST_CHECK_EQ(number, output);
	put(L"Binary", REG_BINARY, &number, sizeof(number));
	size = sizeof(output);
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"Binary", RRF_RT_DWORD, &type, &output, &size));
	TEST_CHECK_EQ(REG_BINARY, type);
	put(L"ShortBinary", REG_BINARY, &number, 3);
	size = sizeof(output);
	TEST_CHECK_EQ(ERROR_DATATYPE_MISMATCH, get(L"ShortBinary", RRF_RT_DWORD, NULL, &output, &size));
	size = sizeof(output);
	output = 0xcccccccc;
	TEST_CHECK_EQ(ERROR_UNSUPPORTED_TYPE, get(L"Number", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, &type, &output, &size));
	TEST_CHECK_EQ(0, output);
	TEST_CHECK_EQ(REG_DWORD, type);
	const WCHAR text[] = L"abc";
	put(L"Text", REG_SZ, text, sizeof(text) - sizeof(WCHAR));
	size = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"Text", RRF_RT_REG_SZ, &type, NULL, &size));
	TEST_CHECK_EQ(sizeof(text), size);
	WCHAR buffer[32];
	memset(buffer, 0xcc, sizeof(buffer));
	size = sizeof(text) - sizeof(WCHAR);
	TEST_CHECK_EQ(ERROR_MORE_DATA, get(L"Text", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, NULL, buffer, &size));
	TEST_CHECK_EQ(sizeof(text), size);
	for (unsigned i = 0; i < sizeof(text) / sizeof(WCHAR) - 1; ++i)
		TEST_CHECK_EQ(0, buffer[i]);
	TEST_CHECK_EQ(0xcccc, buffer[3]);
	size = sizeof(buffer);
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"Text", RRF_RT_REG_SZ, NULL, buffer, &size));
	TEST_CHECK(memcmp(text, buffer, sizeof(text)) == 0);
	TEST_CHECK_EQ(ERROR_UNSUPPORTED_TYPE, get(L"Text", 0, NULL, NULL, NULL));
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"Text", RRF_RT_ANY, NULL, NULL, NULL));
	put(L"Empty", REG_SZ, NULL, 0);
	size = sizeof(buffer);
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"Empty", RRF_RT_REG_SZ, NULL, buffer, &size));
	TEST_CHECK_EQ(sizeof(WCHAR), size);
	TEST_CHECK_EQ(0, buffer[0]);
	const WCHAR multi[] = {L'a', 0, L'b'};
	const WCHAR terminated[] = {L'a', 0, L'b', 0};
	put(L"Multi", REG_MULTI_SZ, multi, sizeof(multi));
	size = sizeof(buffer);
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"Multi", RRF_RT_REG_MULTI_SZ, NULL, buffer, &size));
	TEST_CHECK_EQ(sizeof(terminated), size);
	TEST_CHECK(memcmp(buffer, terminated, sizeof(terminated)) == 0);
	TEST_CHECK(SetEnvironmentVariableW(L"WIBO_REG_GET", L"root"));
	const WCHAR expand[] = L"%WIBO_REG_GET%/tail";
	put(L"Expand", REG_EXPAND_SZ, expand, sizeof(expand));
	size = sizeof(buffer);
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"Expand", RRF_RT_REG_SZ, &type, buffer, &size));
	TEST_CHECK_EQ(REG_SZ, type);
	TEST_CHECK(wcscmp(buffer, L"root/tail") == 0);
	size = sizeof(buffer);
	TEST_CHECK_EQ(ERROR_SUCCESS, get(L"Expand", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, buffer, &size));
	TEST_CHECK_EQ(REG_EXPAND_SZ, type);
	TEST_CHECK(memcmp(expand, buffer, sizeof(expand)) == 0);
	size = sizeof(buffer);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, get(L"Expand", RRF_RT_REG_EXPAND_SZ, NULL, buffer, &size));
	memset(buffer, 0xcc, sizeof(buffer));
	size = sizeof(buffer);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, get(L"Absent", RRF_RT_ANY | RRF_ZEROONFAILURE, NULL, buffer, &size));
	for (unsigned i = 0; i < sizeof(buffer) / sizeof(WCHAR); ++i)
		TEST_CHECK_EQ(0, buffer[i]);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER,
				  get(L"Number", RRF_SUBKEY_WOW6432KEY | RRF_SUBKEY_WOW6464KEY, NULL, NULL, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, get(L"Number", 0, NULL, &output, NULL));
	TEST_CHECK_EQ(ERROR_SUCCESS, RegCloseKey(key));
	return 0;
}
