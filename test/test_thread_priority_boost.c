#include <windows.h>

#include "test_assert.h"

int main(void) {
	HANDLE thread = GetCurrentThread();
	TEST_CHECK(SetThreadPriorityBoost(thread, TRUE));
	TEST_CHECK(SetThreadPriorityBoost(thread, FALSE));
	TEST_CHECK(!SetThreadPriorityBoost((HANDLE)(ULONG_PTR)0x1234, TRUE));
	TEST_CHECK(GetLastError() == ERROR_INVALID_HANDLE);
	return 0;
}
