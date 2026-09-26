#include "test_assert.h"
#include <windows.h>
static volatile LONG count, stop;
static DWORD WINAPI run(void *unused) {
	(void)unused;
	while (!InterlockedCompareExchange(&stop, 0, 0))
		InterlockedIncrement(&count);
	return 7;
}
static LONG readCount(void) { return InterlockedCompareExchange(&count, 0, 0); }
int main(void) {
	DWORD id;
	HANDLE thread = CreateThread(NULL, 0, run, NULL, CREATE_SUSPENDED, &id);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(1, SuspendThread(thread));
	TEST_CHECK_EQ(2, ResumeThread(thread));
	Sleep(20);
	TEST_CHECK_EQ(0, readCount());
	TEST_CHECK_EQ(1, ResumeThread(thread));
	for (unsigned i = 0; i < 100 && !readCount(); ++i)
		Sleep(1);
	TEST_CHECK(readCount() != 0);
	TEST_CHECK_EQ(0, SuspendThread(thread));
	LONG paused = readCount();
	Sleep(20);
	TEST_CHECK_EQ(paused, readCount());
	TEST_CHECK_EQ(1, SuspendThread(thread));
	TEST_CHECK_EQ(2, ResumeThread(thread));
	Sleep(20);
	TEST_CHECK_EQ(paused, readCount());
	HANDLE duplicate = OpenThread(THREAD_SUSPEND_RESUME, FALSE, id);
	TEST_CHECK(duplicate != NULL);
	TEST_CHECK_EQ(1, ResumeThread(duplicate));
	TEST_CHECK(CloseHandle(duplicate));
	Sleep(20);
	TEST_CHECK(readCount() != paused);
	HANDLE restricted = OpenThread(THREAD_QUERY_INFORMATION, FALSE, id);
	TEST_CHECK(restricted != NULL);
	TEST_CHECK_EQ((DWORD)-1, SuspendThread(restricted));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK_EQ((DWORD)-1, ResumeThread(restricted));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(CloseHandle(restricted));
	TEST_CHECK_EQ(0, SuspendThread(thread));
	for (DWORD i = 1; i < 127; ++i)
		TEST_CHECK_EQ(i, SuspendThread(thread));
	TEST_CHECK_EQ((DWORD)-1, SuspendThread(thread));
	TEST_CHECK_EQ(ERROR_SIGNAL_REFUSED, GetLastError());
	for (DWORD i = 127; i > 0; --i)
		TEST_CHECK_EQ(i, ResumeThread(thread));
	TEST_CHECK_EQ(0, ResumeThread(thread));
	InterlockedExchange(&stop, 1);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	DWORD result;
	TEST_CHECK(GetExitCodeThread(thread, &result));
	TEST_CHECK_EQ(7, result);
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK_EQ((DWORD)-1, SuspendThread(NULL));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	return 0;
}
