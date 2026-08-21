#include <windows.h>

#include "test_assert.h"

int main(void) {
	USEROBJECTFLAGS flags;
	DWORD required = 0;
	TEST_CHECK(GetUserObjectInformationA(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), &required));
	TEST_CHECK_EQ(sizeof(flags), required);
	TEST_CHECK_EQ(0, flags.dwFlags & WSF_VISIBLE);

	TEST_CHECK_EQ(RGB(255, 255, 255), GetSysColor(COLOR_WINDOW));
	TEST_CHECK_EQ(RGB(192, 192, 192), GetSysColor(COLOR_BTNFACE));
	TEST_CHECK_EQ(0, GetSysColor(1000));
	return 0;
}
