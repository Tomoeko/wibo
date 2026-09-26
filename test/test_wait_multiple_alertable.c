#include <windows.h>

#include "test_assert.h"

static DWORD callbackThread;
static unsigned callbackCount;
static ULONG_PTR values[4];

static void CALLBACK record(ULONG_PTR value) {
	TEST_CHECK_EQ(callbackThread, GetCurrentThreadId());
	TEST_CHECK(callbackCount < 4);
	values[callbackCount++] = value;
}

typedef struct {
	HANDLE ready;
	HANDLE release;
	HANDLE handles[2];
} WorkerContext;

static DWORD WINAPI wait_worker(void *parameter) {
	WorkerContext *worker = parameter;
	callbackThread = GetCurrentThreadId();
	TEST_CHECK(SetEvent(worker->ready));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(worker->release, 5000));
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, WaitForMultipleObjectsEx(2, worker->handles, FALSE, 5000, TRUE));
	TEST_CHECK_EQ(1, callbackCount);
	TEST_CHECK_EQ(77, values[0]);
	return 0;
}

int main(void) {
	HANDLE handles[2] = {CreateEventW(NULL, FALSE, FALSE, NULL), CreateEventW(NULL, FALSE, FALSE, NULL)};
	TEST_CHECK(handles[0] && handles[1]);
	TEST_CHECK_EQ(WAIT_FAILED, WaitForMultipleObjectsEx(0, handles, FALSE, 0, TRUE));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	HANDLE invalid[2] = {handles[0], (HANDLE)(ULONG_PTR)0x1234};
	TEST_CHECK_EQ(WAIT_FAILED, WaitForMultipleObjectsEx(2, invalid, FALSE, 0, TRUE));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(SetEvent(handles[0]));
	TEST_CHECK(SetEvent(handles[1]));
	SetLastError(0x20001234);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjectsEx(2, handles, FALSE, 0, FALSE));
	TEST_CHECK_EQ(0x20001234, GetLastError());
	TEST_CHECK_EQ(WAIT_OBJECT_0 + 1, WaitForMultipleObjectsEx(2, handles, FALSE, 0, FALSE));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForMultipleObjectsEx(2, handles, FALSE, 0, FALSE));

	TEST_CHECK(SetEvent(handles[0]));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForMultipleObjectsEx(2, handles, TRUE, 0, TRUE));
	TEST_CHECK(SetEvent(handles[1]));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjectsEx(2, handles, TRUE, 0, TRUE));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(handles[0], 0));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(handles[1], 0));

	callbackThread = GetCurrentThreadId();
	TEST_CHECK(QueueUserAPC(record, GetCurrentThread(), 11));
	TEST_CHECK(QueueUserAPC(record, GetCurrentThread(), 22));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForMultipleObjectsEx(2, handles, FALSE, 1, FALSE));
	TEST_CHECK_EQ(0, callbackCount);
	TEST_CHECK(SetEvent(handles[0]));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjectsEx(2, handles, FALSE, 0, TRUE));
	TEST_CHECK_EQ(0, callbackCount);
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, WaitForMultipleObjectsEx(2, handles, FALSE, 0, TRUE));
	TEST_CHECK_EQ(2, callbackCount);
	TEST_CHECK_EQ(11, values[0]);
	TEST_CHECK_EQ(22, values[1]);

	TEST_CHECK(SetEvent(handles[0]));
	TEST_CHECK(QueueUserAPC(record, GetCurrentThread(), 33));
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, WaitForMultipleObjectsEx(2, handles, TRUE, 5000, TRUE));
	TEST_CHECK_EQ(3, callbackCount);
	TEST_CHECK_EQ(33, values[2]);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(handles[0], 0));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForMultipleObjectsEx(2, handles, FALSE, 10, TRUE));

	WorkerContext worker = {
		CreateEventW(NULL, FALSE, FALSE, NULL), CreateEventW(NULL, FALSE, FALSE, NULL), {handles[0], handles[1]}};
	TEST_CHECK(worker.ready && worker.release);
	for (unsigned iteration = 0; iteration < 64; ++iteration) {
		callbackCount = 0;
		HANDLE thread = CreateThread(NULL, 0, wait_worker, &worker, 0, NULL);
		TEST_CHECK(thread != NULL);
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(worker.ready, 5000));
		TEST_CHECK(SetEvent(worker.release));
		if (iteration & 1)
			Sleep(1);
		TEST_CHECK(QueueUserAPC(record, thread, 77));
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
		TEST_CHECK(CloseHandle(thread));
	}
	TEST_CHECK(CloseHandle(worker.ready));
	TEST_CHECK(CloseHandle(worker.release));
	TEST_CHECK(CloseHandle(handles[0]));
	TEST_CHECK(CloseHandle(handles[1]));
	return 0;
}
