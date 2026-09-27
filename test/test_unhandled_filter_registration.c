#include <stdio.h>
#include <windows.h>

#include "test_assert.h"

struct Worker {
	HANDLE start;
	LPTOP_LEVEL_EXCEPTION_FILTER filter;
	LPTOP_LEVEL_EXCEPTION_FILTER previous;
	DWORD error;
	DWORD result;
};

static LONG callbacks;

static LONG WINAPI filterA(EXCEPTION_POINTERS *information) {
	(void)information;
	InterlockedIncrement(&callbacks);
	return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI filterB(EXCEPTION_POINTERS *information) {
	(void)information;
	InterlockedIncrement(&callbacks);
	return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG WINAPI filterC(EXCEPTION_POINTERS *information) {
	(void)information;
	InterlockedIncrement(&callbacks);
	return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI filterD(EXCEPTION_POINTERS *information) {
	(void)information;
	InterlockedExchange(&callbacks, 4);
	return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI workerMain(void *argument) {
	struct Worker *worker = argument;
	if (WaitForSingleObject(worker->start, 5000) != WAIT_OBJECT_0) {
		worker->result = 1;
		return 1;
	}
	SetLastError(0x4321);
	worker->previous = SetUnhandledExceptionFilter(worker->filter);
	worker->error = GetLastError();
	return 0;
}

static int checkRegistration(void) {
	LPTOP_LEVEL_EXCEPTION_FILTER filters[] = {filterA, filterB, filterC, filterD};
	LPTOP_LEVEL_EXCEPTION_FILTER original = SetUnhandledExceptionFilter(NULL);
	struct Worker workers[4] = {0};
	HANDLE threads[4] = {NULL, NULL, NULL, NULL};
	HANDLE start = CreateEventW(NULL, TRUE, FALSE, NULL);
	int result = 1;
	unsigned created = 0;
	if (!start)
		goto cleanup;
	SetLastError(0x4321);
	LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(filterA);
	DWORD error = GetLastError();
	if (previous != NULL || error != 0x4321)
		goto cleanup;
	SetLastError(0x4321);
	previous = SetUnhandledExceptionFilter(filterB);
	error = GetLastError();
	if (previous != filterA || error != 0x4321)
		goto cleanup;
	SetLastError(0x4321);
	previous = SetUnhandledExceptionFilter(NULL);
	error = GetLastError();
	if (previous != filterB || error != 0x4321)
		goto cleanup;
	for (unsigned index = 0; index < 4; ++index) {
		workers[index].start = start;
		workers[index].filter = filters[index];
		threads[index] = CreateThread(NULL, 0, workerMain, &workers[index], 0, NULL);
		if (!threads[index])
			goto cleanup;
		++created;
	}
	if (!SetEvent(start))
		goto cleanup;
	for (unsigned index = 0; index < created; ++index) {
		if (WaitForSingleObject(threads[index], 5000) != WAIT_OBJECT_0)
			goto cleanup;
		if (workers[index].result || workers[index].error != 0x4321)
			goto cleanup;
	}
	SetLastError(0x4321);
	LPTOP_LEVEL_EXCEPTION_FILTER final = SetUnhandledExceptionFilter(NULL);
	error = GetLastError();
	if (error != 0x4321)
		goto cleanup;
	// One exchange chain contains the initial NULL and each of the four installed filters exactly once.
	for (unsigned candidate = 0; candidate < 5; ++candidate) {
		LPTOP_LEVEL_EXCEPTION_FILTER expected = candidate ? filters[candidate - 1] : NULL;
		unsigned occurrences = final == expected;
		for (unsigned index = 0; index < 4; ++index)
			occurrences += workers[index].previous == expected;
		if (occurrences != 1)
			goto cleanup;
	}
	if (callbacks)
		goto cleanup;
	result = 0;
cleanup:
	if (start)
		SetEvent(start);
	for (unsigned index = 0; index < created; ++index) {
		if (WaitForSingleObject(threads[index], 5000) != WAIT_OBJECT_0)
			ExitProcess(2);
		if (!CloseHandle(threads[index]))
			result = 3;
	}
	SetUnhandledExceptionFilter(original);
	if (start && !CloseHandle(start))
		result = 3;
	return result;
}

int main(void) {
	TEST_CHECK_EQ(0, checkRegistration());
	printf("concurrent_setters=4 callbacks=%ld\n", (long)callbacks);
	return 0;
}
