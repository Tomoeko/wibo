#include "test_assert.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <wchar.h>

int main(void) {
	WCHAR name[80];
	TEST_CHECK(swprintf(name, 80, L"Local\\semaphore_%lu_%lu", GetCurrentProcessId(), GetTickCount()) > 0);
	SetLastError(0);
	TEST_CHECK(OpenSemaphoreW(SEMAPHORE_MODIFY_STATE, FALSE, name) == NULL);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	HANDLE original = CreateSemaphoreW(NULL, 0, 2, name);
	TEST_CHECK(original != NULL);
	HANDLE opened = OpenSemaphoreW(SEMAPHORE_MODIFY_STATE, FALSE, name);
	TEST_CHECK(opened != NULL);
	LONG previous = -1;
	TEST_CHECK(ReleaseSemaphore(opened, 1, &previous));
	TEST_CHECK_EQ(0, previous);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(original, 0));
	TEST_CHECK(CloseHandle(opened));
	TEST_CHECK(CloseHandle(original));
	return 0;
}
