#include <windows.h>

#include "test_assert.h"

typedef struct {
	HANDLE semaphore;
	HANDLE stop;
	volatile LONG failures;
} WaitRaceContext;

static DWORD WINAPI wait_worker(void *parameter) {
	WaitRaceContext *context = (WaitRaceContext *)parameter;
	HANDLE handles[2] = {context->semaphore, context->stop};
	for (;;) {
		DWORD result = WaitForMultipleObjects(2, handles, FALSE, 1);
		if (result == WAIT_OBJECT_0 + 1) {
			return 0;
		}
		if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) {
			InterlockedIncrement(&context->failures);
			return 1;
		}
	}
}

int main(void) {
	WaitRaceContext context = {0};
	context.semaphore = CreateSemaphoreA(NULL, 0, 100000, NULL);
	context.stop = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(context.semaphore != NULL);
	TEST_CHECK(context.stop != NULL);

	HANDLE threads[8];
	for (DWORD index = 0; index < 8; ++index) {
		threads[index] = CreateThread(NULL, 0, wait_worker, &context, 0, NULL);
		TEST_CHECK(threads[index] != NULL);
	}
	for (DWORD index = 0; index < 4000; ++index) {
		TEST_CHECK(ReleaseSemaphore(context.semaphore, 1, NULL));
		Sleep(0);
	}
	TEST_CHECK(SetEvent(context.stop));
	TEST_CHECK(WaitForMultipleObjects(8, threads, TRUE, 10000) == WAIT_OBJECT_0);
	TEST_CHECK(context.failures == 0);

	for (DWORD index = 0; index < 8; ++index) {
		TEST_CHECK(CloseHandle(threads[index]));
	}
	TEST_CHECK(CloseHandle(context.stop));
	TEST_CHECK(CloseHandle(context.semaphore));
	return 0;
}
