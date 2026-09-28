#define _WIN32_WINNT 0x0501
#include <windows.h>

#include "test_assert.h"

typedef struct {
	HANDLE fired;
	HANDLE release;
	volatile LONG count;
	volatile LONG wrongKind;
} TimerContext;

static VOID CALLBACK onTimer(PVOID parameter, BOOLEAN fired) {
	TimerContext *context = (TimerContext *)parameter;
	if (!fired)
		InterlockedIncrement(&context->wrongKind);
	InterlockedIncrement(&context->count);
	SetEvent(context->fired);
	if (context->release)
		WaitForSingleObject(context->release, 5000);
}

int main(void) {
	HANDLE queue = CreateTimerQueue();
	TEST_CHECK(queue != NULL);

	TimerContext oneShot = {CreateEventW(NULL, TRUE, FALSE, NULL), NULL, 0, 0};
	TEST_CHECK(oneShot.fired != NULL);
	HANDLE timer = NULL;
	TEST_CHECK(CreateTimerQueueTimer(&timer, queue, onTimer, &oneShot, 3000, 0, WT_EXECUTEDEFAULT));
	TEST_CHECK(timer != NULL);
	TEST_CHECK(ChangeTimerQueueTimer(queue, timer, 20, 0));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(oneShot.fired, 5000));
	TEST_CHECK(DeleteTimerQueueTimer(queue, timer, INVALID_HANDLE_VALUE));
	TEST_CHECK_EQ(1, oneShot.count);
	TEST_CHECK_EQ(0, oneShot.wrongKind);
	TEST_CHECK(CloseHandle(oneShot.fired));

	TimerContext periodic = {CreateEventW(NULL, FALSE, FALSE, NULL), NULL, 0, 0};
	TEST_CHECK(periodic.fired != NULL);
	TEST_CHECK(CreateTimerQueueTimer(&timer, NULL, onTimer, &periodic, 10, 60, WT_EXECUTEDEFAULT));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(periodic.fired, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(periodic.fired, 5000));
	TEST_CHECK(DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE));
	const LONG stopped = periodic.count;
	Sleep(150);
	TEST_CHECK_EQ(stopped, periodic.count);
	TEST_CHECK(stopped >= 2);
	TEST_CHECK_EQ(0, periodic.wrongKind);
	TEST_CHECK(CloseHandle(periodic.fired));

	TimerContext pending = {CreateEventW(NULL, TRUE, FALSE, NULL), CreateEventW(NULL, TRUE, FALSE, NULL), 0, 0};
	HANDLE complete = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(pending.fired && pending.release && complete);
	TEST_CHECK(CreateTimerQueueTimer(&timer, queue, onTimer, &pending, 0, 0, WT_EXECUTELONGFUNCTION));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(pending.fired, 5000));
	BOOL deleted = DeleteTimerQueueTimer(queue, timer, complete);
	if (!deleted)
		TEST_CHECK_EQ(ERROR_IO_PENDING, GetLastError());
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(complete, 0));
	TEST_CHECK(SetEvent(pending.release));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(complete, 5000));
	TEST_CHECK_EQ(1, pending.count);
	TEST_CHECK_EQ(0, pending.wrongKind);
	TEST_CHECK(CloseHandle(complete));
	TEST_CHECK(CloseHandle(pending.release));
	TEST_CHECK(CloseHandle(pending.fired));
	return 0;
}
