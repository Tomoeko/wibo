#include "test_assert.h"
#include <windows.h>

static CRITICAL_SECTION lock;
static CONDITION_VARIABLE work = CONDITION_VARIABLE_INIT;
static CONDITION_VARIABLE changed;
static unsigned ready, permits, completed, turn;

static void sleep_on(CONDITION_VARIABLE *condition) {
	SetLastError(0x71);
	TEST_CHECK(SleepConditionVariableCS(condition, &lock, 5000));
	TEST_CHECK_EQ(0x71, GetLastError());
	TEST_CHECK_EQ(1, lock.RecursionCount);
	TEST_CHECK_U64_EQ(GetCurrentThreadId(), (ULONG_PTR)lock.OwningThread);
}

static DWORD WINAPI consume(void *unused) {
	(void)unused;
	EnterCriticalSection(&lock);
	++ready;
	WakeAllConditionVariable(&changed);
	while (!permits)
		sleep_on(&work);
	--permits;
	++completed;
	WakeAllConditionVariable(&changed);
	LeaveCriticalSection(&lock);
	return 0;
}

static DWORD WINAPI exchange(void *unused) {
	(void)unused;
	EnterCriticalSection(&lock);
	for (unsigned i = 0; i < 200; ++i) {
		while (turn != 1)
			sleep_on(&work);
		turn = 0;
		WakeConditionVariable(&work);
	}
	LeaveCriticalSection(&lock);
	return 0;
}

int main(void) {
	InitializeCriticalSection(&lock);
	InitializeConditionVariable(&changed);
	WakeConditionVariable(&work);
	WakeAllConditionVariable(&work);
	EnterCriticalSection(&lock);
	for (unsigned i = 0; i < 2; ++i) {
		TEST_CHECK(!SleepConditionVariableCS(&work, &lock, i ? 10 : 0));
		TEST_CHECK_EQ(ERROR_TIMEOUT, GetLastError());
		TEST_CHECK_EQ(1, lock.RecursionCount);
		TEST_CHECK_U64_EQ(GetCurrentThreadId(), (ULONG_PTR)lock.OwningThread);
	}
	HANDLE threads[8];
	for (unsigned i = 0; i < 8; ++i) {
		threads[i] = CreateThread(NULL, 0, consume, NULL, 0, NULL);
		TEST_CHECK(threads[i] != NULL);
	}
	while (ready != 8)
		sleep_on(&changed);
	permits = 1;
	WakeConditionVariable(&work);
	while (!completed)
		sleep_on(&changed);
	TEST_CHECK_EQ(1, completed);
	permits = 7;
	WakeAllConditionVariable(&work);
	while (completed != 8)
		sleep_on(&changed);
	LeaveCriticalSection(&lock);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(8, threads, TRUE, 5000));
	for (unsigned i = 0; i < 8; ++i)
		TEST_CHECK(CloseHandle(threads[i]));
	HANDLE thread = CreateThread(NULL, 0, exchange, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	EnterCriticalSection(&lock);
	for (unsigned i = 0; i < 200; ++i) {
		turn = 1;
		WakeConditionVariable(&work);
		while (turn != 0)
			sleep_on(&work);
	}
	LeaveCriticalSection(&lock);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(CloseHandle(thread));
	DeleteCriticalSection(&lock);
	return 0;
}
