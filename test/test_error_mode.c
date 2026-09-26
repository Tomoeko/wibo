#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "test_assert.h"

static HANDLE ready, proceed;

static DWORD WINAPI worker(void *unused) {
	(void)unused;
	TEST_CHECK_EQ(SEM_FAILCRITICALERRORS, GetErrorMode());
	TEST_CHECK_EQ(0, GetThreadErrorMode());
	TEST_CHECK(SetEvent(ready));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(proceed, 5000));
	TEST_CHECK_EQ(SEM_NOOPENFILEERRORBOX, GetErrorMode());
	TEST_CHECK_EQ(0, GetThreadErrorMode());
	DWORD old = 0xdeadbeef;
	SetLastError(77);
	TEST_CHECK(SetThreadErrorMode(SEM_NOOPENFILEERRORBOX, &old));
	TEST_CHECK_EQ(0, old);
	TEST_CHECK_EQ(77, GetLastError());
	TEST_CHECK_EQ(SEM_NOOPENFILEERRORBOX, GetThreadErrorMode());
	return 0;
}

int main(void) {
	UINT initial = SetErrorMode(SEM_FAILCRITICALERRORS);
	DWORD initialThread = GetThreadErrorMode(), old = 0xdeadbeef;
	SetLastError(77);
	TEST_CHECK(SetThreadErrorMode(SEM_NOGPFAULTERRORBOX, &old));
	TEST_CHECK_EQ(initialThread, old);
	TEST_CHECK_EQ(77, GetLastError());
	TEST_CHECK_EQ(SEM_FAILCRITICALERRORS, GetErrorMode());
	ready = CreateEventA(NULL, TRUE, FALSE, NULL);
	proceed = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(ready && proceed);
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(ready, 5000));
	TEST_CHECK_EQ(SEM_FAILCRITICALERRORS, SetErrorMode(SEM_NOOPENFILEERRORBOX));
	TEST_CHECK(SetEvent(proceed));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	DWORD exitCode;
	TEST_CHECK(GetExitCodeThread(thread, &exitCode));
	TEST_CHECK_EQ(0, exitCode);
	TEST_CHECK_EQ(SEM_NOGPFAULTERRORBOX, GetThreadErrorMode());
	const DWORD valid[] = {0, 1, 2, 3, 0x8000, 0x8003};
	DWORD previous = SEM_NOGPFAULTERRORBOX;
	for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
		SetLastError(77);
		TEST_CHECK(SetThreadErrorMode(valid[i], &old));
		TEST_CHECK_EQ(previous, old);
		TEST_CHECK_EQ(77, GetLastError());
		TEST_CHECK_EQ(valid[i], GetThreadErrorMode());
		previous = valid[i];
	}
	const DWORD invalid[] = {4, 7, 8, 0x8004, 0xffffffff};
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
		old = 0xdeadbeef;
		TEST_CHECK(!SetThreadErrorMode(invalid[i], &old));
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
		TEST_CHECK_EQ(0xdeadbeef, old);
		TEST_CHECK_EQ(previous, GetThreadErrorMode());
	}
	TEST_CHECK(SetThreadErrorMode(0, NULL));
	SetErrorMode(SEM_FAILCRITICALERRORS);
	TEST_CHECK_EQ(0, GetThreadErrorMode());
	TEST_CHECK(SetThreadErrorMode(initialThread, NULL));
	SetErrorMode(initial);
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(ready));
	TEST_CHECK(CloseHandle(proceed));
	return 0;
}
