// clang-format off: MinGW's timeapi.h requires the Windows base types first.
#include <windows.h>
#include <timeapi.h>
// clang-format on

#include "test_assert.h"

int main(void) {
	TEST_CHECK_EQ(TIMERR_NOERROR, timeBeginPeriod(1));
	TEST_CHECK(timeGetTime() != (DWORD)-1);
	TEST_CHECK_EQ(TIMERR_NOERROR, timeEndPeriod(1));
	TEST_CHECK_EQ(TIMERR_NOCANDO, timeBeginPeriod(0));
	TEST_CHECK_EQ(TIMERR_NOCANDO, timeEndPeriod(0));
	return 0;
}
