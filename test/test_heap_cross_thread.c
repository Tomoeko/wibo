#include <windows.h>

#include "test_assert.h"

typedef struct HeapThreadContext {
	HANDLE heap;
	HANDLE startEvent;
	void *mainAllocation;
	void *workerAllocation;
	DWORD error;
} HeapThreadContext;

static DWORD WINAPI heapWorker(void *parameter) {
	HeapThreadContext *context = (HeapThreadContext *)parameter;
	if (!HeapFree(context->heap, 0, context->mainAllocation)) {
		context->error = GetLastError();
		return 1;
	}

	BYTE *allocation = (BYTE *)HeapAlloc(context->heap, HEAP_ZERO_MEMORY, 64);
	if (!allocation) {
		context->error = GetLastError();
		return 2;
	}
	for (SIZE_T index = 0; index < 64; ++index) {
		if (allocation[index] != 0) {
			context->error = ERROR_INVALID_DATA;
			HeapFree(context->heap, 0, allocation);
			return 3;
		}
		allocation[index] = (BYTE)index;
	}

	allocation = (BYTE *)HeapReAlloc(context->heap, 0, allocation, 256);
	if (!allocation) {
		context->error = GetLastError();
		return 4;
	}
	context->workerAllocation = allocation;
	return 0;
}

static DWORD WINAPI heapStressWorker(void *parameter) {
	HeapThreadContext *context = (HeapThreadContext *)parameter;
	if (WaitForSingleObject(context->startEvent, 5000) != WAIT_OBJECT_0) {
		context->error = ERROR_TIMEOUT;
		return 1;
	}
	for (DWORD iteration = 0; iteration < 2000; ++iteration) {
		SIZE_T initialSize = 32 + (iteration & 127);
		BYTE *allocation = (BYTE *)HeapAlloc(context->heap, HEAP_ZERO_MEMORY, initialSize);
		if (!allocation) {
			context->error = GetLastError();
			return 2;
		}
		allocation[0] = (BYTE)iteration;
		allocation = (BYTE *)HeapReAlloc(context->heap, 0, allocation, initialSize + 256);
		if (!allocation) {
			context->error = GetLastError();
			return 3;
		}
		if (!HeapFree(context->heap, 0, allocation)) {
			context->error = GetLastError();
			return 4;
		}
	}
	return 0;
}

int main(void) {
	HeapThreadContext context = {0};
	context.heap = HeapCreate(0, 4096, 0);
	TEST_CHECK_MSG(context.heap != NULL, "HeapCreate failed: %lu", (unsigned long)GetLastError());

	context.mainAllocation = HeapAlloc(context.heap, 0, 128);
	TEST_CHECK_MSG(context.mainAllocation != NULL, "main HeapAlloc failed: %lu", (unsigned long)GetLastError());

	HANDLE thread = CreateThread(NULL, 0, heapWorker, &context, 0, NULL);
	TEST_CHECK_MSG(thread != NULL, "CreateThread failed: %lu", (unsigned long)GetLastError());
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));

	DWORD exitCode = STILL_ACTIVE;
	TEST_CHECK(GetExitCodeThread(thread, &exitCode));
	TEST_CHECK_MSG(exitCode == 0, "worker failed: exit=%lu error=%lu", (unsigned long)exitCode,
				   (unsigned long)context.error);
	TEST_CHECK(context.workerAllocation != NULL);
	TEST_CHECK(HeapSize(context.heap, 0, context.workerAllocation) >= 256);
	TEST_CHECK(HeapFree(context.heap, 0, context.workerAllocation));
	TEST_CHECK(CloseHandle(thread));

	HANDLE startEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(startEvent != NULL);
	HeapThreadContext stressContexts[12] = {0};
	HANDLE stressThreads[12] = {0};
	for (DWORD index = 0; index < 12; ++index) {
		stressContexts[index].heap = context.heap;
		stressContexts[index].startEvent = startEvent;
		stressThreads[index] = CreateThread(NULL, 0, heapStressWorker, &stressContexts[index], 0, NULL);
		TEST_CHECK(stressThreads[index] != NULL);
	}
	TEST_CHECK(SetEvent(startEvent));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(12, stressThreads, TRUE, 30000));
	for (DWORD index = 0; index < 12; ++index) {
		DWORD stressExitCode = STILL_ACTIVE;
		TEST_CHECK(GetExitCodeThread(stressThreads[index], &stressExitCode));
		TEST_CHECK_MSG(stressExitCode == 0, "stress worker %lu failed: exit=%lu error=%lu", (unsigned long)index,
					   (unsigned long)stressExitCode, (unsigned long)stressContexts[index].error);
		TEST_CHECK(CloseHandle(stressThreads[index]));
	}
	TEST_CHECK(CloseHandle(startEvent));
	TEST_CHECK(HeapDestroy(context.heap));
	return 0;
}
