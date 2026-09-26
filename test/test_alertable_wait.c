#include "test_assert.h"
#include <windows.h>

static DWORD callbackThread;
static ULONG_PTR values[8];
static int count;
static void CALLBACK record(ULONG_PTR value) {
	TEST_CHECK_EQ(callbackThread, GetCurrentThreadId());
	values[count++] = value;
}
static DWORD WINAPI worker(void *parameter) {
	HANDLE ready = (HANDLE)parameter;
	callbackThread = GetCurrentThreadId();
	TEST_CHECK(SetEvent(ready));
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, SleepEx(5000, TRUE));
	return 0;
}
static DWORD WINAPI initial_worker(void *parameter) {
	(void)parameter;
	TEST_CHECK_EQ(1, count);
	TEST_CHECK_EQ(77, values[0]);
	return 0;
}
int main(void) {
	callbackThread = GetCurrentThreadId();
#ifdef _WIN64
	const ULONG_PTR payload = 0x12345678fedcba98ULL;
#else
	const ULONG_PTR payload = 0xfedcba98U;
#endif
	HANDLE denied;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &denied, SYNCHRONIZE, FALSE, 0));
	TEST_CHECK(!QueueUserAPC(record, denied, 99));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(CloseHandle(denied));
	HANDLE self;
	TEST_CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, THREAD_SET_CONTEXT,
							   FALSE, 0));
	TEST_CHECK(QueueUserAPC(record, self, 11));
	TEST_CHECK(QueueUserAPC(record, GetCurrentThread(), payload));
	TEST_CHECK_EQ(0, SleepEx(1, FALSE));
	TEST_CHECK_EQ(0, count);
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, SleepEx(0, TRUE));
	TEST_CHECK_EQ(2, count);
	TEST_CHECK_EQ(11, values[0]);
	TEST_CHECK_U64_EQ(payload, values[1]);
	TEST_CHECK_EQ(0, SleepEx(0, TRUE));
	HANDLE event = CreateEventW(NULL, FALSE, TRUE, NULL);
	TEST_CHECK(event != NULL);
	TEST_CHECK(QueueUserAPC(record, self, 33));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObjectEx(event, 0, TRUE));
	TEST_CHECK_EQ(2, count);
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, WaitForSingleObjectEx(event, 5000, TRUE));
	TEST_CHECK(SetEvent(event));
	TEST_CHECK_EQ(3, count);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObjectEx(event, 0, TRUE));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObjectEx(event, 10, TRUE));
	TEST_CHECK(CloseHandle(event));
	LARGE_INTEGER due;
	due.QuadPart = -100000;
	HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
	TEST_CHECK(timer != NULL);
	TEST_CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObjectEx(timer, 5000, TRUE));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObjectEx(timer, 0, TRUE));
	TEST_CHECK(CloseHandle(timer));
	count = 0;
	HANDLE ready = CreateEventW(NULL, FALSE, FALSE, NULL);
	TEST_CHECK(ready != NULL);
	HANDLE thread = CreateThread(NULL, 0, worker, ready, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(ready, 5000));
	TEST_CHECK(QueueUserAPC(record, thread, 44));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK_EQ(1, count);
	TEST_CHECK_EQ(44, values[0]);
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(ready));
	count = 0;
	DWORD id;
	thread = CreateThread(NULL, 0, initial_worker, NULL, CREATE_SUSPENDED, &id);
	TEST_CHECK(thread != NULL);
	callbackThread = id;
	TEST_CHECK(QueueUserAPC(record, thread, 77));
	TEST_CHECK_EQ(1, ResumeThread(thread));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(self));
	return 0;
}
