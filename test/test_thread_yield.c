#include <windows.h>

#include "test_assert.h"

static volatile LONG progress, stop;
static DWORD WINAPI worker(void *unused) {
	(void)unused;
	while (!InterlockedCompareExchange(&stop, 0, 0)) {
		InterlockedIncrement(&progress);
		SwitchToThread();
	}
	return 0;
}

int main(void) {
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	ULONGLONG deadline = GetTickCount64() + 5000;
	while (InterlockedCompareExchange(&progress, 0, 0) < 100 && GetTickCount64() < deadline) {
		SetLastError(77);
		BOOL result = SwitchToThread();
		TEST_CHECK(result == FALSE || result == TRUE);
		TEST_CHECK_EQ(77, GetLastError());
	}
	TEST_CHECK(InterlockedCompareExchange(&progress, 0, 0) >= 100);
	InterlockedExchange(&stop, 1);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	DWORD exitCode;
	TEST_CHECK(GetExitCodeThread(thread, &exitCode));
	TEST_CHECK_EQ(0, exitCode);
	TEST_CHECK(CloseHandle(thread));
	return 0;
}
