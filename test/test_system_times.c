#include <windows.h>

#include "test_assert.h"

static ULONGLONG ticks(FILETIME value) { return ((ULONGLONG)value.dwHighDateTime << 32) | value.dwLowDateTime; }

int main(void) {
	FILETIME idle, kernel, user, laterIdle, laterKernel, laterUser;
	SetLastError(77);
	TEST_CHECK(GetSystemTimes(&idle, &kernel, &user));
	TEST_CHECK_EQ(77, GetLastError());
	TEST_CHECK(ticks(kernel) >= ticks(idle));
	TEST_CHECK(ticks(kernel) + ticks(user) > 0);
	Sleep(100);
	TEST_CHECK(GetSystemTimes(&laterIdle, &laterKernel, &laterUser));
	TEST_CHECK(ticks(laterIdle) >= ticks(idle));
	TEST_CHECK(ticks(laterKernel) >= ticks(kernel));
	TEST_CHECK(ticks(laterUser) >= ticks(user));
	TEST_CHECK(ticks(laterKernel) >= ticks(laterIdle));
	TEST_CHECK(ticks(laterKernel) + ticks(laterUser) - ticks(kernel) - ticks(user) >= 400000);
	TEST_CHECK(GetSystemTimes(NULL, NULL, NULL));
	TEST_CHECK(GetSystemTimes(&idle, NULL, NULL));
	TEST_CHECK(GetSystemTimes(NULL, &kernel, NULL));
	TEST_CHECK(GetSystemTimes(NULL, NULL, &user));
	return 0;
}
