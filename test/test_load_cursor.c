#include "test_assert.h"
#include <windows.h>

static DWORD WINAPI worker(LPVOID unused) {
	(void)unused;
	HCURSOR text = LoadCursorA(NULL, IDC_IBEAM);
	HCURSOR arrow = LoadCursorA(NULL, IDC_ARROW);
	TEST_CHECK(SetCursor(text) == NULL);
	TEST_CHECK(SetCursor(arrow) == text);
	TEST_CHECK(SetCursor(NULL) == arrow);
	return 0;
}

int main(void) {
	HCURSOR arrow = LoadCursorA(NULL, IDC_ARROW);
	TEST_CHECK(arrow != NULL);
	TEST_CHECK(arrow == LoadCursorW(NULL, MAKEINTRESOURCEW(32512)));
	HCURSOR text = LoadCursorW(NULL, MAKEINTRESOURCEW(32513));
	TEST_CHECK(text != NULL && text != arrow);
	TEST_CHECK(text == LoadCursorA(NULL, IDC_IBEAM));
	HICON application = LoadIconA(NULL, IDI_APPLICATION);
	TEST_CHECK(application != NULL && (HANDLE)application != (HANDLE)arrow);
	TEST_CHECK(application == LoadIconW(NULL, MAKEINTRESOURCEW(32512)));
	TEST_CHECK(!CloseHandle(application));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(40000)) == NULL);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK(SetEnvironmentVariableA("WIBO_HEADLESS", NULL));
		SetLastError(0);
		TEST_CHECK(SetCursor(arrow) == NULL);
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK(SetEnvironmentVariableA("WIBO_HEADLESS", "1"));
	}
	HCURSOR original = SetCursor(arrow);
	TEST_CHECK(SetCursor(text) == arrow);
	TEST_CHECK(SetCursor(arrow) == text);
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, INFINITE));
	DWORD result = 1;
	TEST_CHECK(GetExitCodeThread(thread, &result));
	TEST_CHECK_EQ(0, result);
	CloseHandle(thread);
	TEST_CHECK(SetCursor(NULL) == arrow);
	SetCursor(original);
	TEST_CHECK(!CloseHandle(arrow));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	HANDLE duplicate = NULL;
	TEST_CHECK(
		!DuplicateHandle(GetCurrentProcess(), arrow, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(!SetHandleInformation(arrow, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(LoadCursorW(NULL, L"missing.fixture.cursor") == NULL);
	TEST_CHECK(LoadCursorA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(40000)) == NULL);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK(LoadCursorA(NULL, MAKEINTRESOURCEA(65534)) == NULL);
		TEST_CHECK_EQ(ERROR_INVALID_DATA, GetLastError());
		TEST_CHECK(LoadCursorA(NULL, MAKEINTRESOURCEA(65535)) == NULL);
		TEST_CHECK_EQ(ERROR_INVALID_DATA, GetLastError());
	}
	return 0;
}
