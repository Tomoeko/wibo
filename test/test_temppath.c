#include "test_assert.h"

#include <string.h>
#include <wchar.h>
#include <windows.h>

int main(void) {
	WCHAR wide_path[32768], small[2] = {L'x', L'y'};
	DWORD wide_length = GetTempPathW(32768, wide_path);
	TEST_CHECK(wide_length > 0 && wide_length < 32768);
	TEST_CHECK_EQ(wide_length, wcslen(wide_path));
	TEST_CHECK_EQ(L'\\', wide_path[wide_length - 1]);
	TEST_CHECK_EQ(wide_length + 1, GetTempPathW(0, NULL));
	TEST_CHECK_EQ(wide_length + 1, GetTempPathW(1, small));
	TEST_CHECK_EQ(L'y', small[1]);

	char buffer[MAX_PATH];
	DWORD len = GetTempPathA(sizeof(buffer), buffer);
	TEST_CHECK(len > 0 && len < sizeof(buffer));
	TEST_CHECK_EQ(len, strlen(buffer));
	TEST_CHECK_EQ('\\', buffer[len - 1]);

	char too_small[MAX_PATH];
	memset(too_small, 0xCC, sizeof(too_small));
	DWORD required = GetTempPathA(len, too_small);
	TEST_CHECK_EQ(len + 1, required);

	return 0;
}
