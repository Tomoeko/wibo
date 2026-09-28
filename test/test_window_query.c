#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "test_assert.h"

int main(void) {
	const HWND invalidWindow = (HWND)(UINT_PTR)1;
	DWORD processId = 0xa5a5a5a5;
	SetLastError(0x20001234);
	TEST_CHECK_EQ(0, GetWindowThreadProcessId(NULL, &processId));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	TEST_CHECK_EQ(0xa5a5a5a5, processId);
	SetLastError(0x20001234);
	TEST_CHECK_EQ(0, GetWindowThreadProcessId(invalidWindow, &processId));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	TEST_CHECK_EQ(0xa5a5a5a5, processId);
	SetLastError(0x20001234);
	TEST_CHECK_EQ(0, GetWindowThreadProcessId(NULL, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(!IsWindow(NULL));
	TEST_CHECK_EQ(0x20001234, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(!IsWindow(invalidWindow));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(!IsWindowVisible(NULL));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(!IsWindowVisible(invalidWindow));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(!IsWindowEnabled(NULL));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(!IsWindowEnabled(invalidWindow));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	return 0;
}
