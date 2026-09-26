#include "test_assert.h"
#include <windows.h>

static HANDLE gate, copy;
static DWORD WINAPI worker(void *unused) {
	(void)unused;
	TEST_CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &copy, 0, FALSE,
							   DUPLICATE_SAME_ACCESS));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(gate, 5000));
	return 37;
}
static void state(HANDLE thread, DWORD id, DWORD expected) {
	DWORD code = 0;
	TEST_CHECK_EQ(id, GetThreadId(thread));
	TEST_CHECK(GetExitCodeThread(thread, &code));
	TEST_CHECK_EQ(expected, code);
}
int main(void) {
	TEST_CHECK(OpenThread(THREAD_QUERY_INFORMATION, FALSE, 0) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(OpenThread(THREAD_QUERY_INFORMATION, FALSE, 0xFFFFFFFF) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(0x71);
	HANDLE mainThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
	TEST_CHECK(mainThread != NULL);
	TEST_CHECK_EQ(0x71, GetLastError());
	state(mainThread, GetCurrentThreadId(), STILL_ACTIVE);
	TEST_CHECK(CloseHandle(mainThread));
	gate = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(gate != NULL);
	DWORD id;
	HANDLE original = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, &id);
	TEST_CHECK(original != NULL);
	TEST_CHECK(CloseHandle(original));
	HANDLE opened = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | SYNCHRONIZE, TRUE, id);
	TEST_CHECK(opened != NULL);
	DWORD flags;
	TEST_CHECK(GetHandleInformation(opened, &flags));
	TEST_CHECK_EQ(HANDLE_FLAG_INHERIT, flags);
	state(opened, id, STILL_ACTIVE);
	HANDLE limited = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
	TEST_CHECK(limited != NULL);
	TEST_CHECK_EQ((DWORD)-1, ResumeThread(limited));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	HANDLE denied = OpenThread(SYNCHRONIZE, FALSE, id);
	TEST_CHECK(denied != NULL);
	DWORD untouched = 0x51;
	TEST_CHECK(!GetExitCodeThread(denied, &untouched));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK_EQ(0x51, untouched);
	TEST_CHECK_EQ(0, GetThreadId(denied));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK_EQ(WAIT_FAILED, WaitForSingleObject(limited, 0));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK_EQ(1, ResumeThread(opened));
	TEST_CHECK(SetEvent(gate));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(opened, 5000));
	TEST_CHECK(copy != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(copy, 5000));
	state(copy, id, 37);
	state(limited, id, 37);
	HANDLE exited = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
	TEST_CHECK(exited != NULL);
	state(exited, id, 37);
	TEST_CHECK(CloseHandle(copy));
	TEST_CHECK(CloseHandle(exited));
	TEST_CHECK(CloseHandle(opened));
	TEST_CHECK(CloseHandle(limited));
	TEST_CHECK(CloseHandle(denied));
	TEST_CHECK(CloseHandle(gate));
	Sleep(20);
	TEST_CHECK(OpenThread(THREAD_QUERY_INFORMATION, FALSE, id) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	return 0;
}
