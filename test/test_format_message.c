#include "test_assert.h"
#include <windows.h>

int main(void) {
	const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	char narrow[512];
	WCHAR wide[512];
	DWORD length = FormatMessageA(flags, NULL, ERROR_ACCESS_DENIED, 0x409, narrow, sizeof(narrow), NULL);
	TEST_CHECK(length > 0 && length < sizeof(narrow));
	TEST_CHECK_EQ(length, strlen(narrow));
	TEST_CHECK(strstr(narrow, "denied") != NULL);
	DWORD wideLength = FormatMessageW(flags, NULL, ERROR_ACCESS_DENIED, 0x409, wide, 512, NULL);
	TEST_CHECK_EQ(length, wideLength);
	for (DWORD index = 0; index <= length; ++index)
		TEST_CHECK_EQ((BYTE)narrow[index], wide[index]);
	char *allocated = NULL;
	TEST_CHECK_EQ(length, FormatMessageA(flags | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, ERROR_ACCESS_DENIED, 0x409,
										 (LPSTR)&allocated, 512, NULL));
	TEST_CHECK(allocated != NULL);
	TEST_CHECK(strcmp(allocated, narrow) == 0);
	TEST_CHECK(LocalFree(allocated) == NULL);
	WCHAR *allocatedWide = NULL;
	TEST_CHECK_EQ(wideLength, FormatMessageW(flags | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, ERROR_ACCESS_DENIED, 0x409,
											 (LPWSTR)&allocatedWide, 512, NULL));
	TEST_CHECK(allocatedWide != NULL);
	TEST_CHECK(LocalFree(allocatedWide) == NULL);
	TEST_CHECK_EQ(0, FormatMessageA(flags, NULL, ERROR_ACCESS_DENIED, 0x409, narrow, 1, NULL));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK_EQ(0, FormatMessageA(flags, NULL, 0xFFFF1234, 0x409, narrow, sizeof(narrow), NULL));
	TEST_CHECK_EQ(ERROR_MR_MID_NOT_FOUND, GetLastError());
	return 0;
}
