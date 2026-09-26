#include <windows.h>

#include "test_assert.h"

int main(void) {
	TEST_CHECK(!AttachConsole(0));
	DWORD error = GetLastError();
	TEST_CHECK(error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_HANDLE);
	TEST_CHECK(!AttachConsole(0xfffffffe));
	error = GetLastError();
	TEST_CHECK(error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_HANDLE);
	TEST_CHECK(!AttachConsole(GetCurrentProcessId()));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	return 0;
}
