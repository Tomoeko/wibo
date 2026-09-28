#define _WIN32_WINNT 0x0601
#include <objbase.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

int main(void) {
	const GUID value = {0x1234abcd, 0x5678, 0x9ef0, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}};
	const WCHAR expected[] = L"{1234ABCD-5678-9EF0-1234-56789ABCDEF0}";
	const DWORD seed = 4321;
	WCHAR text[42];
	for (unsigned index = 0; index < 42; ++index)
		text[index] = 0xa5a5;

	SetLastError(seed);
	TEST_CHECK_EQ(0, StringFromGUID2(&value, text, 38));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(0xa5a5, text[0]);
	SetLastError(seed);
	TEST_CHECK_EQ(39, StringFromGUID2(&value, text, 39));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK(wcscmp(expected, text) == 0);
	TEST_CHECK_EQ(0xa5a5, text[39]);

	LPOLESTR allocated = NULL;
	SetLastError(seed);
	TEST_CHECK_EQ(S_OK, StringFromCLSID(&value, &allocated));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK(allocated != NULL && wcscmp(expected, allocated) == 0);
	CoTaskMemFree(allocated);
	allocated = NULL;
	SetLastError(seed);
	TEST_CHECK_EQ(S_OK, StringFromIID(&value, &allocated));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK(allocated != NULL && wcscmp(expected, allocated) == 0);
	CoTaskMemFree(allocated);

	GUID parsed = {0};
	SetLastError(seed);
	TEST_CHECK_EQ(S_OK, IIDFromString(expected, &parsed));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK(memcmp(&value, &parsed, sizeof(value)) == 0);
	parsed = value;
	SetLastError(seed);
	TEST_CHECK_EQ(S_OK, IIDFromString(NULL, &parsed));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(0, parsed.Data1);
	SetLastError(seed);
	TEST_CHECK_EQ(E_INVALIDARG, IIDFromString(L"invalid", &parsed));
	TEST_CHECK_EQ(seed, GetLastError());
	return 0;
}
