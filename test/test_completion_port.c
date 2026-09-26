#include "test_assert.h"
#include <windows.h>
static void empty(HANDLE port, DWORD timeout, DWORD expected) {
	DWORD n = 0x71;
	ULONG_PTR key = 0x81;
	OVERLAPPED *ov = (OVERLAPPED *)(ULONG_PTR)0x91;
	TEST_CHECK(!GetQueuedCompletionStatus(port, &n, &key, &ov, timeout));
	TEST_CHECK_EQ(expected, GetLastError());
	TEST_CHECK(ov == NULL);
	TEST_CHECK_EQ(0x71, n);
	TEST_CHECK_U64_EQ(0x81, key);
}
static void packet(HANDLE port, DWORD expected, ULONG_PTR wanted, OVERLAPPED *operation) {
	DWORD n = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *ov = NULL;
	SetLastError(0x51);
	TEST_CHECK(GetQueuedCompletionStatus(port, &n, &key, &ov, 5000));
	TEST_CHECK_EQ(expected, n);
	TEST_CHECK_U64_EQ(wanted, key);
	TEST_CHECK(ov == operation);
	TEST_CHECK_EQ(0x51, GetLastError());
}
static HANDLE waitingPort, started;
static HANDLE dequeued;
static volatile LONG proceed, received;
static DWORD WINAPI limitedWait(void *unused) {
	(void)unused;
	TEST_CHECK(SetEvent(started));
	while (!proceed) {
	}
	packet(waitingPort, 2, 2, NULL);
	InterlockedExchange(&received, 1);
	TEST_CHECK(SetEvent(dequeued));
	return 0;
}
static DWORD WINAPI closedWait(void *unused) {
	(void)unused;
	TEST_CHECK(SetEvent(started));
	empty(waitingPort, INFINITE, ERROR_ABANDONED_WAIT_0);
	return 0;
}
int main(void) {
	SetLastError(0x51);
	HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	TEST_CHECK(port != NULL);
	TEST_CHECK_EQ(0x51, GetLastError());
	empty(port, 0, WAIT_TIMEOUT);
	empty(port, 1, WAIT_TIMEOUT);
	OVERLAPPED post[3] = {0};
	post[0].Internal = 0x71;
	const ULONG_PTR high = ((ULONG_PTR)1 << (sizeof(ULONG_PTR) * 8 - 1));
	for (unsigned i = 0; i < 3; ++i)
		TEST_CHECK(PostQueuedCompletionStatus(port, i + 1, high + i, &post[i]));
	for (unsigned i = 0; i < 3; ++i)
		packet(port, i + 1, high + i, &post[i]);
	TEST_CHECK_EQ(0x71, post[0].Internal);
	TEST_CHECK(PostQueuedCompletionStatus(port, 0, high, NULL));
	packet(port, 0, high, NULL);
	TEST_CHECK(CreateIoCompletionPort(INVALID_HANDLE_VALUE, port, 0, 1) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	HANDLE file = CreateFileA("wibo_completion_fixture.tmp", GENERIC_READ | GENERIC_WRITE,
							  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
							  FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	TEST_CHECK(CreateIoCompletionPort(file, port, high + 7, 0) == port);
	TEST_CHECK(CreateIoCompletionPort(file, port, high + 8, 0) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	OVERLAPPED write = {0};
	BOOL issued = WriteFile(file, "abc", 3, NULL, &write);
	TEST_CHECK(issued || GetLastError() == ERROR_IO_PENDING);
	packet(port, 3, high + 7, &write);
	OVERLAPPED read = {0};
	BYTE data[3];
	issued = ReadFile(file, data, 3, NULL, &read);
	TEST_CHECK(issued || GetLastError() == ERROR_IO_PENDING);
	packet(port, 3, high + 7, &read);
	TEST_CHECK(memcmp(data, "abc", 3) == 0);
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	OVERLAPPED suppressed = {0};
	suppressed.hEvent = (HANDLE)((ULONG_PTR)event | 1);
	issued = ReadFile(file, data, 3, NULL, &suppressed);
	TEST_CHECK(issued || GetLastError() == ERROR_IO_PENDING);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(event, 5000));
	DWORD count;
	TEST_CHECK(GetOverlappedResult(file, &suppressed, &count, TRUE));
	TEST_CHECK_EQ(3, count);
	empty(port, 0, WAIT_TIMEOUT);
	OVERLAPPED end = {0};
	end.Offset = 20;
	issued = ReadFile(file, data, 3, NULL, &end);
	TEST_CHECK(!issued);
	TEST_CHECK(GetLastError() == ERROR_IO_PENDING || GetLastError() == ERROR_HANDLE_EOF);
	ULONG_PTR key;
	OVERLAPPED *ov;
	count = 0x71;
	TEST_CHECK(!GetQueuedCompletionStatus(port, &count, &key, &ov, 5000));
	TEST_CHECK_EQ(ERROR_HANDLE_EOF, GetLastError());
	TEST_CHECK_EQ(0, count);
	TEST_CHECK_U64_EQ(high + 7, key);
	TEST_CHECK(ov == &end);
	TEST_CHECK(CloseHandle(event));
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(CloseHandle(port));
	waitingPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	started = CreateEventW(NULL, TRUE, FALSE, NULL);
	dequeued = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(waitingPort && started && dequeued);
	HANDLE limited = CreateThread(NULL, 0, limitedWait, NULL, 0, NULL);
	TEST_CHECK(limited != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(started, 5000));
	TEST_CHECK(PostQueuedCompletionStatus(waitingPort, 1, 1, NULL));
	TEST_CHECK(PostQueuedCompletionStatus(waitingPort, 2, 2, NULL));
	packet(waitingPort, 1, 1, NULL);
	InterlockedExchange(&proceed, 1);
	const DWORD observe = GetTickCount();
	while ((DWORD)(GetTickCount() - observe) < 30) {
	}
	// The compatibility baseline does not enforce the advertised concurrency limit.
	TEST_CHECK_EQ(getenv("WIBO_FIXTURE_RUNTIME") ? 0 : 1, received);
	// A genuine blocking wait releases the associated thread's concurrency slot.
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(dequeued, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(limited, 5000));
	TEST_CHECK(CloseHandle(limited));
	TEST_CHECK(CloseHandle(started));
	TEST_CHECK(CloseHandle(dequeued));
	TEST_CHECK(CloseHandle(waitingPort));
	waitingPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	started = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(waitingPort && started);
	HANDLE thread = CreateThread(NULL, 0, closedWait, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(started, 5000));
	Sleep(20);
	TEST_CHECK(CloseHandle(waitingPort));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(started));
	return 0;
}
