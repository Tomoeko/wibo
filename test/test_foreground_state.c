#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdlib.h>

#include "test_assert.h"

int main(void) {
	const BOOL noWindowBackend = getenv("WIBO_EXPECT_NO_WINDOW_BACKEND") != NULL;
	SetLastError(0x20001234);
	const HWND foreground = GetForegroundWindow();
	TEST_CHECK_EQ(0x20001234, GetLastError());
	if (noWindowBackend)
		TEST_CHECK(foreground == NULL);
	SetLastError(0x20001234);
	TEST_CHECK(GetActiveWindow() == NULL);
	TEST_CHECK_EQ(0x20001234, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(GetFocus() == NULL);
	TEST_CHECK_EQ(0x20001234, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(SetFocus(NULL) == NULL);
	TEST_CHECK_EQ(0x20001234, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(SetActiveWindow(NULL) == NULL);
	TEST_CHECK_EQ(0x20001234, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(SetFocus((HWND)(UINT_PTR)1) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(SetActiveWindow((HWND)(UINT_PTR)1) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(!SetForegroundWindow(NULL));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(!SetForegroundWindow((HWND)(UINT_PTR)1));
	TEST_CHECK_EQ(ERROR_INVALID_WINDOW_HANDLE, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(AllowSetForegroundWindow(ASFW_ANY));
	TEST_CHECK_EQ(0x20001234, GetLastError());
	SetLastError(0x20001234);
	TEST_CHECK(AllowSetForegroundWindow(0));
	TEST_CHECK_EQ(0x20001234, GetLastError());
	if (noWindowBackend) {
		TEST_CHECK(!AllowSetForegroundWindow(1));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	}
	return 0;
}
