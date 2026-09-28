#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdlib.h>

#include "test_assert.h"

static int isHex(WCHAR character) {
	return (character >= L'0' && character <= L'9') || (character >= L'A' && character <= L'F') ||
	       (character >= L'a' && character <= L'f');
}

int main(void) {
	const int unavailable = getenv("WIBO_EXPECT_KEYBOARD_UNAVAILABLE") != NULL;
	const int malformed = getenv("WIBO_EXPECT_KEYBOARD_MALFORMED") != NULL;
	const int wiboOnly = getenv("WIBO_EXPECT_KEYBOARD_GAPS") != NULL;
	const DWORD seed = 0x4321;
	SetLastError(seed);
	const HKL layout = GetKeyboardLayout(0);
	if (unavailable || malformed) {
		TEST_CHECK(layout == NULL);
		TEST_CHECK_EQ(unavailable ? ERROR_NOT_SUPPORTED : ERROR_INVALID_DATA, GetLastError());
		return 0;
	}
	TEST_CHECK(layout != NULL);
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	TEST_CHECK(GetKeyboardLayout(GetCurrentThreadId()) == layout);
	TEST_CHECK_EQ(seed, GetLastError());

	WCHAR name[12];
	for (unsigned index = 0; index < 12; ++index) name[index] = 0xa5a5;
	SetLastError(seed);
	TEST_CHECK(GetKeyboardLayoutNameW(name));
	TEST_CHECK_EQ(seed, GetLastError());
	for (unsigned index = 0; index < 8; ++index) TEST_CHECK(isHex(name[index]));
	TEST_CHECK_EQ(0, name[8]);
	for (unsigned index = 9; index < 12; ++index) TEST_CHECK_EQ(0xa5a5, name[index]);
	const int usEnglish = lstrcmpW(name, L"00000409") == 0;

	(void)VkKeyScanExW(L'a', layout);
	SetLastError(seed);
	const SHORT lower = VkKeyScanExW(L'a', layout);
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	const SHORT upper = VkKeyScanExW(L'A', layout);
	TEST_CHECK_EQ(seed, GetLastError());
	if (usEnglish) {
		TEST_CHECK_EQ(0x0041, lower);
		TEST_CHECK_EQ(0x0141, upper);
		SetLastError(seed);
		TEST_CHECK_EQ(-1, VkKeyScanExW(0x4e2d, layout));
		TEST_CHECK_EQ(seed, GetLastError());
	}

	SetLastError(seed);
	const UINT ansiScan = MapVirtualKeyA('A', MAPVK_VK_TO_VSC);
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	const UINT unicodeScan = MapVirtualKeyW('A', MAPVK_VK_TO_VSC);
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	const UINT explicitScan = MapVirtualKeyExA('A', MAPVK_VK_TO_VSC, layout);
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(ansiScan, unicodeScan);
	TEST_CHECK_EQ(unicodeScan, explicitScan);
	if (usEnglish) TEST_CHECK_EQ(0x1e, ansiScan);

	BYTE state[256] = {0};
	WCHAR output[12];
	for (unsigned index = 0; index < 12; ++index) output[index] = 0xa5a5;
	SetLastError(seed);
	const int count = ToUnicode('A', ansiScan, state, output, 8, 4);
	TEST_CHECK_EQ(seed, GetLastError());
	if (usEnglish) {
		TEST_CHECK_EQ(1, count);
		TEST_CHECK_EQ(L'a', output[0]);
	}
	for (unsigned index = 8; index < 12; ++index) TEST_CHECK_EQ(0xa5a5, output[index]);

	if (wiboOnly) {
		SetLastError(seed);
		TEST_CHECK_EQ(-1, VkKeyScanExW(L'a', (HKL)(ULONG_PTR)1));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		SetLastError(seed);
		TEST_CHECK_EQ(0, MapVirtualKeyExA('A', MAPVK_VK_TO_VSC, (HKL)(ULONG_PTR)1));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		for (unsigned index = 0; index < 12; ++index) output[index] = 0xa5a5;
		SetLastError(seed);
		TEST_CHECK_EQ(0, ToUnicode('A', ansiScan, state, output, 8, 0));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		for (unsigned index = 0; index < 12; ++index) TEST_CHECK_EQ(0xa5a5, output[index]);
	}
	return 0;
}
